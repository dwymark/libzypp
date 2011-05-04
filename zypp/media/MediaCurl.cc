/*---------------------------------------------------------------------\
|                          ____ _   __ __ ___                          |
|                         |__  / \ / / . \ . \                         |
|                           / / \ V /|  _/  _/                         |
|                          / /__ | | | | | |                           |
|                         /_____||_| |_| |_|                           |
|                                                                      |
\---------------------------------------------------------------------*/
/** \file zypp/media/MediaCurl.cc
 *
*/

#include <iostream>
#include <list>

#include "zypp/base/Logger.h"
#include "zypp/ExternalProgram.h"
#include "zypp/base/String.h"
#include "zypp/base/Gettext.h"
#include "zypp/base/Sysconfig.h"
#include "zypp/base/Gettext.h"

#include "zypp/media/MediaCurl.h"
#include "zypp/media/proxyinfo/ProxyInfos.h"
#include "zypp/media/ProxyInfo.h"
#include "zypp/media/MediaUserAuth.h"
#include "zypp/media/CredentialManager.h"
#include "zypp/media/CurlConfig.h"
#include "zypp/thread/Once.h"
#include "zypp/Target.h"
#include "zypp/ZYppFactory.h"

#include <cstdlib>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <boost/format.hpp>

#define  DETECT_DIR_INDEX       0
#define  CONNECT_TIMEOUT        60
#define  TRANSFER_TIMEOUT       60 * 3
#define  TRANSFER_TIMEOUT_MAX   60 * 60


using namespace std;
using namespace zypp::base;

namespace
{
  zypp::thread::OnceFlag g_InitOnceFlag = PTHREAD_ONCE_INIT;
  zypp::thread::OnceFlag g_FreeOnceFlag = PTHREAD_ONCE_INIT;

  extern "C" void _do_free_once()
  {
    curl_global_cleanup();
  }

  extern "C" void globalFreeOnce()
  {
    zypp::thread::callOnce(g_FreeOnceFlag, _do_free_once);
  }

  extern "C" void _do_init_once()
  {
    CURLcode ret = curl_global_init( CURL_GLOBAL_ALL );
    if ( ret != 0 )
    {
      WAR << "curl global init failed" << endl;
    }

    //
    // register at exit handler ?
    // this may cause trouble, because we can protect it
    // against ourself only.
    // if the app sets an atexit handler as well, it will
    // cause a double free while the second of them runs.
    //
    //std::atexit( globalFreeOnce);
  }

  inline void globalInitOnce()
  {
    zypp::thread::callOnce(g_InitOnceFlag, _do_init_once);
  }

  int log_curl(CURL *curl, curl_infotype info,
               char *ptr, size_t len, void *max_lvl)
  {
    std::string pfx(" ");
    long        lvl = 0;
    switch( info)
    {
      case CURLINFO_TEXT:       lvl = 1; pfx = "*"; break;
      case CURLINFO_HEADER_IN:  lvl = 2; pfx = "<"; break;
      case CURLINFO_HEADER_OUT: lvl = 2; pfx = ">"; break;
      default:                                      break;
    }
    if( lvl > 0 && max_lvl != NULL && lvl <= *((long *)max_lvl))
    {
      std::string                            msg(ptr, len);
      std::list<std::string>                 lines;
      std::list<std::string>::const_iterator line;
      zypp::str::split(msg, std::back_inserter(lines), "\r\n");
      for(line = lines.begin(); line != lines.end(); ++line)
      {
        DBG << pfx << " " << *line << endl;
      }
    }
    return 0;
  }

  static size_t
  log_redirects_curl(
      void *ptr, size_t size, size_t nmemb, void *stream)
  {
    // INT << "got header: " << string((char *)ptr, ((char*)ptr) + size*nmemb) << endl;

    char * lstart = (char *)ptr, * lend = (char *)ptr;
    size_t pos = 0;
    size_t max = size * nmemb;
    while (pos + 1 < max)
    {
      // get line
      for (lstart = lend; *lend != '\n' && pos < max; ++lend, ++pos);

      // look for "Location"
      string line(lstart, lend);
      if (line.find("Location") != string::npos)
      {
        DBG << "redirecting to " << line << endl;
        return max;
      }

      // continue with the next line
      if (pos + 1 < max)
      {
        ++lend;
        ++pos;
      }
      else
        break;
    }

    return max;
  }
}

namespace zypp {
  namespace media {

  namespace {
    struct ProgressData
    {
      ProgressData(const long _timeout, const zypp::Url &_url = zypp::Url(),
                   callback::SendReport<DownloadProgressReport> *_report=NULL)
        : timeout(_timeout)
        , reached(false)
        , report(_report)
        , drate_period(-1)
        , dload_period(0)
        , secs(0)
        , drate_avg(-1)
        , ltime( time(NULL))
        , dload( 0)
        , uload( 0)
        , url(_url)
      {}
      long                                          timeout;
      bool                                          reached;
      callback::SendReport<DownloadProgressReport> *report;
      // download rate of the last period (cca 1 sec)
      double                                        drate_period;
      // bytes downloaded at the start of the last period
      double                                        dload_period;
      // seconds from the start of the download
      long                                          secs;
      // average download rate
      double                                        drate_avg;
      // last time the progress was reported
      time_t                                        ltime;
      // bytes downloaded at the moment the progress was last reported
      double                                        dload;
      // bytes uploaded at the moment the progress was last reported
      double                                        uload;
      zypp::Url                                     url;
    };

    ///////////////////////////////////////////////////////////////////

    inline void escape( string & str_r,
                        const char char_r, const string & escaped_r ) {
      for ( string::size_type pos = str_r.find( char_r );
            pos != string::npos; pos = str_r.find( char_r, pos ) ) {
              str_r.replace( pos, 1, escaped_r );
            }
    }

    inline string escapedPath( string path_r ) {
      escape( path_r, ' ', "%20" );
      return path_r;
    }

    inline string unEscape( string text_r ) {
      char * tmp = curl_unescape( text_r.c_str(), 0 );
      string ret( tmp );
      curl_free( tmp );
      return ret;
    }

  }

/**
 * Fills the settings structure using options passed on the url
 * for example ?timeout=x&proxy=foo
 */
void fillSettingsFromUrl( const Url &url, TransferSettings &s )
{
    std::string param(url.getQueryParam("timeout"));
    if( !param.empty())
    {
      long num = str::strtonum<long>(param);
      if( num >= 0 && num <= TRANSFER_TIMEOUT_MAX)
          s.setTimeout(num);
    }

    if ( ! url.getUsername().empty() )
    {
        s.setUsername(url.getUsername());
        if ( url.getPassword().size() )
            s.setPassword(url.getPassword());
    }
    else
    {
        // if there is no username, set anonymous auth
        if ( url.getScheme() == "ftp" && s.username().empty() )
            s.setAnonymousAuth();
    }

    if ( url.getScheme() == "https" )
    {
        s.setVerifyPeerEnabled(false);
        s.setVerifyHostEnabled(false);

        std::string verify( url.getQueryParam("ssl_verify"));
        if( verify.empty() ||
            verify == "yes")
        {
            s.setVerifyPeerEnabled(true);
            s.setVerifyHostEnabled(true);
        }
        else if( verify == "no")
        {
            s.setVerifyPeerEnabled(false);
            s.setVerifyHostEnabled(false);
        }
        else
        {
            std::vector<std::string>                 flags;
            std::vector<std::string>::const_iterator flag;
            str::split( verify, std::back_inserter(flags), ",");
            for(flag = flags.begin(); flag != flags.end(); ++flag)
            {
                if( *flag == "host")
                    s.setVerifyHostEnabled(true);
                else if( *flag == "peer")
                    s.setVerifyPeerEnabled(true);
                else
                    ZYPP_THROW(MediaBadUrlException(url, "Unknown ssl_verify flag"));
            }
        }
    }

    Pathname ca_path = Pathname(url.getQueryParam("ssl_capath")).asString();
    if( ! ca_path.empty())
    {
        if( !PathInfo(ca_path).isDir() || !Pathname(ca_path).absolute())
            ZYPP_THROW(MediaBadUrlException(url, "Invalid ssl_capath path"));
        else
            s.setCertificateAuthoritiesPath(ca_path);
    }

    string proxy = url.getQueryParam( "proxy" );
    if ( ! proxy.empty() )
    {
        string proxyport( url.getQueryParam( "proxyport" ) );
        if ( ! proxyport.empty() ) {
            proxy += ":" + proxyport;
        }
        s.setProxy(proxy);
        s.setProxyEnabled(true);
    }
}

/**
 * Reads the system proxy configuration and fills the settings
 * structure proxy information
 */
void fillSettingsSystemProxy( const Url&url, TransferSettings &s )
{
    ProxyInfo proxy_info (ProxyInfo::ImplPtr(new ProxyInfoSysconfig("proxy")));
    s.setProxyEnabled( proxy_info.useProxyFor( url ) );
    if ( s.proxyEnabled() )
      s.setProxy(proxy_info.proxy(url.getScheme()));
}

Pathname MediaCurl::_cookieFile = "/var/lib/YaST2/cookies";

/**
 * initialized only once, this gets the anonymous id
 * from the target, which we pass in the http header
 */
static const char *const anonymousIdHeader()
{
  // we need to add the release and identifier to the
  // agent string.
  // The target could be not initialized, and then this information
  // is guessed.
  static const std::string _value(
      str::trim( str::form(
          "X-ZYpp-AnonymousId: %s",
          Target::anonymousUniqueId( Pathname()/*guess root*/ ).c_str() ) )
  );
  return _value.c_str();
}

/**
 * initialized only once, this gets the distribution flavor
 * from the target, which we pass in the http header
 */
static const char *const distributionFlavorHeader()
{
  // we need to add the release and identifier to the
  // agent string.
  // The target could be not initialized, and then this information
  // is guessed.
  static const std::string _value(
      str::trim( str::form(
          "X-ZYpp-DistributionFlavor: %s",
          Target::distributionFlavor( Pathname()/*guess root*/ ).c_str() ) )
  );
  return _value.c_str();
}

/**
 * initialized only once, this gets the agent string
 * which also includes the curl version
 */
static const char *const agentString()
{
  // we need to add the release and identifier to the
  // agent string.
  // The target could be not initialized, and then this information
  // is guessed.
  static const std::string _value(
    str::form(
       "ZYpp %s (curl %s) %s"
       , VERSION
       , curl_version_info(CURLVERSION_NOW)->version
       , Target::targetDistribution( Pathname()/*guess root*/ ).c_str()
    )
  );
  return _value.c_str();
}

// we use this define to unbloat code as this C setting option
// and catching exception is done frequently.
/** \todo deprecate SET_OPTION and use the typed versions below. */
#define SET_OPTION(opt,val) do { \
    ret = curl_easy_setopt ( _curl, opt, val ); \
    if ( ret != 0) { \
      disconnectFrom(); \
      ZYPP_THROW(MediaCurlSetOptException(_url, _curlError)); \
    } \
  } while ( false )

#define SET_OPTION_OFFT(opt,val) SET_OPTION(opt,(curl_off_t)val)
#define SET_OPTION_LONG(opt,val) SET_OPTION(opt,(LONG)val)
#define SET_OPTION_VOID(opt,val) SET_OPTION(opt,(void*)val)

MediaCurl::MediaCurl( const Url &      url_r,
                      const Pathname & attach_point_hint_r )
    : MediaHandler( url_r, attach_point_hint_r,
                    "/", // urlpath at attachpoint
                    true ), // does_download
      _curl( NULL ),
      _customHeaders(0L)
{
  _curlError[0] = '\0';
  _curlDebug = 0L;

  MIL << "MediaCurl::MediaCurl(" << url_r << ", " << attach_point_hint_r << ")" << endl;

  globalInitOnce();

  if( !attachPoint().empty())
  {
    PathInfo ainfo(attachPoint());
    Pathname apath(attachPoint() + "XXXXXX");
    char    *atemp = ::strdup( apath.asString().c_str());
    char    *atest = NULL;
    if( !ainfo.isDir() || !ainfo.userMayRWX() ||
         atemp == NULL || (atest=::mkdtemp(atemp)) == NULL)
    {
      WAR << "attach point " << ainfo.path()
          << " is not useable for " << url_r.getScheme() << endl;
      setAttachPoint("", true);
    }
    else if( atest != NULL)
      ::rmdir(atest);

    if( atemp != NULL)
      ::free(atemp);
  }
}

void MediaCurl::setCookieFile( const Pathname &fileName )
{
  _cookieFile = fileName;
}

///////////////////////////////////////////////////////////////////

void MediaCurl::attachTo (bool next)
{
  if ( next )
    ZYPP_THROW(MediaNotSupportedException(_url));

  if ( !_url.isValid() )
    ZYPP_THROW(MediaBadUrlException(_url));

  CurlConfig curlconf;
  CurlConfig::parseConfig(curlconf); // parse ~/.curlrc

  curl_version_info_data *curl_info = NULL;
  curl_info = curl_version_info(CURLVERSION_NOW);
  // curl_info does not need any free (is static)
  if (curl_info->protocols)
  {
    const char * const *proto;
    std::string        scheme( _url.getScheme());
    bool               found = false;
    for(proto=curl_info->protocols; !found && *proto; ++proto)
    {
      if( scheme == std::string((const char *)*proto))
        found = true;
    }
    if( !found)
    {
      std::string msg("Unsupported protocol '");
      msg += scheme;
      msg += "'";
      ZYPP_THROW(MediaBadUrlException(_url, msg));
    }
  }

  if( !isUseableAttachPoint(attachPoint()))
  {
    std::string mountpoint = createAttachPoint().asString();

    if( mountpoint.empty())
      ZYPP_THROW( MediaBadAttachPointException(url()));

    setAttachPoint( mountpoint, true);
  }

  disconnectFrom(); // clean _curl if needed
  _curl = curl_easy_init();
  if ( !_curl ) {
    ZYPP_THROW(MediaCurlInitException(_url));
  }

  {
    char *ptr = getenv("ZYPP_MEDIA_CURL_DEBUG");
    _curlDebug = (ptr && *ptr) ? str::strtonum<long>( ptr) : 0L;
    if( _curlDebug > 0)
    {
      curl_easy_setopt( _curl, CURLOPT_VERBOSE, 1);
      curl_easy_setopt( _curl, CURLOPT_DEBUGFUNCTION, log_curl);
      curl_easy_setopt( _curl, CURLOPT_DEBUGDATA, &_curlDebug);
    }
  }

  curl_easy_setopt(_curl, CURLOPT_HEADERFUNCTION, log_redirects_curl);

  CURLcode ret = curl_easy_setopt( _curl, CURLOPT_ERRORBUFFER, _curlError );
  if ( ret != 0 ) {
    disconnectFrom();
    ZYPP_THROW(MediaCurlSetOptException(_url, "Error setting error buffer"));
  }

  SET_OPTION(CURLOPT_FAILONERROR,true);
  SET_OPTION(CURLOPT_NOSIGNAL, 1);

  // reset settings in case we are re-attaching
  _settings.reset();

  // add custom headers
  _settings.addHeader(anonymousIdHeader());
  _settings.addHeader(distributionFlavorHeader());
  _settings.addHeader("Pragma:");

  _settings.setTimeout(TRANSFER_TIMEOUT);
  _settings.setConnectTimeout(CONNECT_TIMEOUT);

  _settings.setUserAgentString(agentString());

  // fill some settings from url query parameters
  try
  {
      fillSettingsFromUrl(_url, _settings);
  }
  catch ( const MediaException &e )
  {
      disconnectFrom();
      ZYPP_RETHROW(e);
  }

  // if the proxy was not set by url, then look
  if ( _settings.proxy().empty() )
  {
      // at the system proxy settings
      fillSettingsSystemProxy(_url, _settings);
  }

  DBG << "Proxy: " << (_settings.proxy().empty() ? "-none-" : _settings.proxy()) << endl;

 /**
  * Connect timeout
  */
  SET_OPTION(CURLOPT_CONNECTTIMEOUT, _settings.connectTimeout());

  // follow any Location: header that the server sends as part of
  // an HTTP header (#113275)
  SET_OPTION(CURLOPT_FOLLOWLOCATION, true);
  // 3 redirects seem to be too few in some cases (bnc #465532)
  SET_OPTION(CURLOPT_MAXREDIRS, 6L);

  if ( _url.getScheme() == "https" )
  {
    // restrict following of redirections from https to https only
    SET_OPTION( CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS );

    if( _settings.verifyPeerEnabled() ||
        _settings.verifyHostEnabled() )
    {
      SET_OPTION(CURLOPT_CAPATH, _settings.certificateAuthoritiesPath().c_str());
    }

    SET_OPTION(CURLOPT_SSL_VERIFYPEER, _settings.verifyPeerEnabled() ? 1L : 0L);
    SET_OPTION(CURLOPT_SSL_VERIFYHOST, _settings.verifyHostEnabled() ? 2L : 0L);
  }

  SET_OPTION(CURLOPT_USERAGENT, _settings.userAgentString().c_str() );

  /*---------------------------------------------------------------*
   CURLOPT_USERPWD: [user name]:[password]

   Url::username/password -> CURLOPT_USERPWD
   If not provided, anonymous FTP identification
   *---------------------------------------------------------------*/

  if ( _settings.userPassword().size() )
  {
    SET_OPTION(CURLOPT_USERPWD, unEscape(_settings.userPassword()).c_str());

    //FIXME, we leave this here for now, as it does not make sense yet
    // to refactor it to the fill settings from url function

    // HTTP authentication type
    if(_url.getScheme() == "http" || _url.getScheme() == "https")
    {
      string use_auth = _url.getQueryParam("auth");
      if( use_auth.empty())
        use_auth = "digest,basic";

      try
      {
        long auth = CurlAuthData::auth_type_str2long(use_auth);
        if( auth != CURLAUTH_NONE)
        {
          DBG << "Enabling HTTP authentication methods: " << use_auth
              << " (CURLOPT_HTTPAUTH=" << auth << ")" << std::endl;

          SET_OPTION(CURLOPT_HTTPAUTH, auth);
        }
      }
      catch (MediaException & ex_r)
      {
        string auth_hint = getAuthHint();

        DBG << "Rethrowing as MediaUnauthorizedException. auth hint: '"
            << auth_hint << "'" << endl;

        ZYPP_THROW(MediaUnauthorizedException(
          _url, ex_r.msg(), _curlError, auth_hint
        ));
      }
    }
  }

  if ( _settings.proxyEnabled() )
  {
    if ( ! _settings.proxy().empty() )
    {
      SET_OPTION(CURLOPT_PROXY, _settings.proxy().c_str());
      /*---------------------------------------------------------------*
        CURLOPT_PROXYUSERPWD: [user name]:[password]

        Url::option(proxyuser and proxypassword) -> CURLOPT_PROXYUSERPWD
        If not provided, $HOME/.curlrc is evaluated
        *---------------------------------------------------------------*/

      string proxyuserpwd = _settings.proxyUserPassword();

      if ( proxyuserpwd.empty() )
      {
        if (curlconf.proxyuserpwd.empty())
          DBG << "~/.curlrc does not contain the proxy-user option" << endl;
        else
        {
          proxyuserpwd = curlconf.proxyuserpwd;
          DBG << "using proxy-user from ~/.curlrc" << endl;
        }
      }

      proxyuserpwd = unEscape( proxyuserpwd );
      if ( ! proxyuserpwd.empty() )
        SET_OPTION(CURLOPT_PROXYUSERPWD, proxyuserpwd.c_str());
    }
  }

  /** Speed limits */
  if ( _settings.minDownloadSpeed() != 0 )
  {
      SET_OPTION(CURLOPT_LOW_SPEED_LIMIT, _settings.minDownloadSpeed());
      // default to 10 seconds at low speed
      SET_OPTION(CURLOPT_LOW_SPEED_TIME, 10);
  }

  if ( _settings.maxDownloadSpeed() != 0 )
      SET_OPTION_OFFT(CURLOPT_MAX_RECV_SPEED_LARGE, _settings.maxDownloadSpeed());

  /*---------------------------------------------------------------*
   *---------------------------------------------------------------*/

  _currentCookieFile = _cookieFile.asString();
  if ( str::strToBool( _url.getQueryParam( "cookies" ), true ) )
    SET_OPTION(CURLOPT_COOKIEFILE, _currentCookieFile.c_str() );
  else
    MIL << "No cookies requested" << endl;
  SET_OPTION(CURLOPT_COOKIEJAR, _currentCookieFile.c_str() );
  SET_OPTION(CURLOPT_PROGRESSFUNCTION, &progressCallback );
  SET_OPTION(CURLOPT_NOPROGRESS, false );

  // bnc #306272
  SET_OPTION(CURLOPT_PROXY_TRANSFER_MODE, 1 );

  // append settings custom headers to curl
  for ( TransferSettings::Headers::const_iterator it = _settings.headersBegin();
        it != _settings.headersEnd();
        ++it )
  {

      _customHeaders = curl_slist_append(_customHeaders, it->c_str());
      if ( !_customHeaders )
          ZYPP_THROW(MediaCurlInitException(_url));
  }

  SET_OPTION(CURLOPT_HTTPHEADER, _customHeaders);

  // FIXME: need a derived class to propelly compare url's
  MediaSourceRef media( new MediaSource(_url.getScheme(), _url.asString()));
  setMediaSource(media);
}

bool
MediaCurl::checkAttachPoint(const Pathname &apoint) const
{
  return MediaHandler::checkAttachPoint( apoint, true, true);
}

///////////////////////////////////////////////////////////////////

void MediaCurl::disconnectFrom()
{
  if ( _customHeaders )
  {
    curl_slist_free_all(_customHeaders);
    _customHeaders = 0L;
  }

  if ( _curl )
  {
    curl_easy_cleanup( _curl );
    _curl = NULL;
  }
}

///////////////////////////////////////////////////////////////////

void MediaCurl::releaseFrom( const std::string & ejectDev )
{
  disconnect();
}

static Url getFileUrl(const Url & url, const Pathname & filename)
{
  Url newurl(url);
  string path = url.getPathName();
  if ( !path.empty() && path != "/" && *path.rbegin() == '/' &&
       filename.absolute() )
  {
    // If url has a path with trailing slash, remove the leading slash from
    // the absolute file name
    path += filename.asString().substr( 1, filename.asString().size() - 1 );
  }
  else if ( filename.relative() )
  {
    // Add trailing slash to path, if not already there
    if (path.empty()) path = "/";
    else if (*path.rbegin() != '/' ) path += "/";
    // Remove "./" from begin of relative file name
    path += filename.asString().substr( 2, filename.asString().size() - 2 );
  }
  else
  {
    path += filename.asString();
  }

  newurl.setPathName(path);
  return newurl;
}

///////////////////////////////////////////////////////////////////

void MediaCurl::getFile( const Pathname & filename ) const
{
    // Use absolute file name to prevent access of files outside of the
    // hierarchy below the attach point.
    getFileCopy(filename, localPath(filename).absolutename());
}

///////////////////////////////////////////////////////////////////

void MediaCurl::getFileCopy( const Pathname & filename , const Pathname & target) const
{
  callback::SendReport<DownloadProgressReport> report;

  Url fileurl(getFileUrl(_url, filename));

  bool retry = false;

  do
  {
    try
    {
      doGetFileCopy(filename, target, report);
      retry = false;
    }
    // retry with proper authentication data
    catch (MediaUnauthorizedException & ex_r)
    {
      if(authenticate(ex_r.hint(), !retry))
        retry = true;
      else
      {
        report->finish(fileurl, zypp::media::DownloadProgressReport::ACCESS_DENIED, ex_r.asUserHistory());
        ZYPP_RETHROW(ex_r);
      }
    }
    // unexpected exception
    catch (MediaException & excpt_r)
    {
      // FIXME: error number fix
      report->finish(fileurl, zypp::media::DownloadProgressReport::ERROR, excpt_r.asUserHistory());
      ZYPP_RETHROW(excpt_r);
    }
  }
  while (retry);

  report->finish(fileurl, zypp::media::DownloadProgressReport::NO_ERROR, "");
}

///////////////////////////////////////////////////////////////////

bool MediaCurl::getDoesFileExist( const Pathname & filename ) const
{
  bool retry = false;

  do
  {
    try
    {
      return doGetDoesFileExist( filename );
    }
    // authentication problem, retry with proper authentication data
    catch (MediaUnauthorizedException & ex_r)
    {
      if(authenticate(ex_r.hint(), !retry))
        retry = true;
      else
        ZYPP_RETHROW(ex_r);
    }
    // unexpected exception
    catch (MediaException & excpt_r)
    {
      ZYPP_RETHROW(excpt_r);
    }
  }
  while (retry);

  return false;
}

///////////////////////////////////////////////////////////////////

void MediaCurl::evaluateCurlCode( const Pathname &filename,
                                  CURLcode code,
                                  bool timeout_reached ) const
{
  Url url(getFileUrl(_url, filename));

  if ( code != 0 )
  {
    std::string err;
    try
    {
      switch ( code )
      {
      case CURLE_UNSUPPORTED_PROTOCOL:
      case CURLE_URL_MALFORMAT:
      case CURLE_URL_MALFORMAT_USER:
          err = " Bad URL";
          break;
      case CURLE_LOGIN_DENIED:
          ZYPP_THROW(
              MediaUnauthorizedException(url, "Login failed.", _curlError, ""));
          break;
      case CURLE_HTTP_RETURNED_ERROR:
      {
        long httpReturnCode = 0;
        CURLcode infoRet = curl_easy_getinfo( _curl,
                                              CURLINFO_RESPONSE_CODE,
                                              &httpReturnCode );
        if ( infoRet == CURLE_OK )
        {
          string msg = "HTTP response: " + str::numstring( httpReturnCode );
          switch ( httpReturnCode )
          {
          case 401:
          {
            string auth_hint = getAuthHint();

            DBG << msg << " Login failed (URL: " << url.asString() << ")" << std::endl;
            DBG << "MediaUnauthorizedException auth hint: '" << auth_hint << "'" << std::endl;

            ZYPP_THROW(MediaUnauthorizedException(
                           url, "Login failed.", _curlError, auth_hint
                           ));
          }

          case 503: // service temporarily unavailable (bnc #462545)
            ZYPP_THROW(MediaTemporaryProblemException(url));
          case 504: // gateway timeout
            ZYPP_THROW(MediaTimeoutException(url));
          case 403:
          {
            string msg403;
            if (url.asString().find("novell.com") != string::npos)
              msg403 = _("Visit the Novell Customer Center to check whether your registration is valid and has not expired.");
            ZYPP_THROW(MediaForbiddenException(url, msg403));
          }
          case 404:
              ZYPP_THROW(MediaFileNotFoundException(_url, filename));
          }

          DBG << msg << " (URL: " << url.asString() << ")" << std::endl;
          ZYPP_THROW(MediaCurlException(url, msg, _curlError));
        }
        else
        {
          string msg = "Unable to retrieve HTTP response:";
          DBG << msg << " (URL: " << url.asString() << ")" << std::endl;
          ZYPP_THROW(MediaCurlException(url, msg, _curlError));
        }
      }
      break;
      case CURLE_FTP_COULDNT_RETR_FILE:
      case CURLE_REMOTE_FILE_NOT_FOUND:
      case CURLE_FTP_ACCESS_DENIED:
        err = "File not found";
        ZYPP_THROW(MediaFileNotFoundException(_url, filename));
        break;
      case CURLE_BAD_PASSWORD_ENTERED:
      case CURLE_FTP_USER_PASSWORD_INCORRECT:
          err = "Login failed";
          break;
      case CURLE_COULDNT_RESOLVE_PROXY:
      case CURLE_COULDNT_RESOLVE_HOST:
      case CURLE_COULDNT_CONNECT:
      case CURLE_FTP_CANT_GET_HOST:
        err = "Connection failed";
        break;
      case CURLE_WRITE_ERROR:
        err = "Write error";
        break;
      case CURLE_PARTIAL_FILE:
      case CURLE_ABORTED_BY_CALLBACK:
      case CURLE_OPERATION_TIMEDOUT:
        if( timeout_reached)
        {
          err  = "Timeout reached";
          ZYPP_THROW(MediaTimeoutException(url));
        }
        else
        {
          err = "User abort";
        }
        break;
      case CURLE_SSL_PEER_CERTIFICATE:
      default:
        err = "Unrecognized error";
        break;
      }

      // uhm, no 0 code but unknown curl exception
      ZYPP_THROW(MediaCurlException(url, err, _curlError));
    }
    catch (const MediaException & excpt_r)
    {
      ZYPP_RETHROW(excpt_r);
    }
  }
  else
  {
    // actually the code is 0, nothing happened
  }
}

///////////////////////////////////////////////////////////////////

bool MediaCurl::doGetDoesFileExist( const Pathname & filename ) const
{
  DBG << filename.asString() << endl;

  if(!_url.isValid())
    ZYPP_THROW(MediaBadUrlException(_url));

  if(_url.getHost().empty())
    ZYPP_THROW(MediaBadUrlEmptyHostException(_url));

  Url url(getFileUrl(_url, filename));

  DBG << "URL: " << url.asString() << endl;
    // Use URL without options and without username and passwd
    // (some proxies dislike them in the URL).
    // Curl seems to need the just scheme, hostname and a path;
    // the rest was already passed as curl options (in attachTo).
  Url curlUrl( url );

  // Use asString + url::ViewOptions instead?
  curlUrl.setUsername( "" );
  curlUrl.setPassword( "" );
  curlUrl.setPathParams( "" );
  curlUrl.setQueryString( "" );
  curlUrl.setFragment( "" );

  //
    // See also Bug #154197 and ftp url definition in RFC 1738:
    // The url "ftp://user@host/foo/bar/file" contains a path,
    // that is relative to the user's home.
    // The url "ftp://user@host//foo/bar/file" (or also with
    // encoded slash as %2f) "ftp://user@host/%2ffoo/bar/file"
    // contains an absolute path.
  //
  string urlBuffer( curlUrl.asString());
  CURLcode ret = curl_easy_setopt( _curl, CURLOPT_URL,
                                   urlBuffer.c_str() );
  if ( ret != 0 ) {
    ZYPP_THROW(MediaCurlSetOptException(url, _curlError));
  }

  // instead of returning no data with NOBODY, we return
  // little data, that works with broken servers, and
  // works for ftp as well, because retrieving only headers
  // ftp will return always OK code ?
  // See http://curl.haxx.se/docs/knownbugs.html #58
  if (  _url.getScheme() == "http" ||  _url.getScheme() == "https" )
    ret = curl_easy_setopt( _curl, CURLOPT_NOBODY, 1 );
  else
    ret = curl_easy_setopt( _curl, CURLOPT_RANGE, "0-1" );

  if ( ret != 0 ) {
    curl_easy_setopt( _curl, CURLOPT_NOBODY, NULL );
    curl_easy_setopt( _curl, CURLOPT_RANGE, NULL );
    /* yes, this is why we never got to get NOBODY working before,
       because setting it changes this option too, and we also
       need to reset it
       See: http://curl.haxx.se/mail/archive-2005-07/0073.html
    */
    curl_easy_setopt( _curl, CURLOPT_HTTPGET, 1 );
    ZYPP_THROW(MediaCurlSetOptException(url, _curlError));
  }

  FILE *file = ::fopen( "/dev/null", "w" );
  if ( !file ) {
      ::fclose(file);
      ERR << "fopen failed for /dev/null" << endl;
      curl_easy_setopt( _curl, CURLOPT_NOBODY, NULL );
      curl_easy_setopt( _curl, CURLOPT_RANGE, NULL );
      /* yes, this is why we never got to get NOBODY working before,
       because setting it changes this option too, and we also
       need to reset it
       See: http://curl.haxx.se/mail/archive-2005-07/0073.html
      */
      curl_easy_setopt( _curl, CURLOPT_HTTPGET, 1 );
      if ( ret != 0 ) {
          ZYPP_THROW(MediaCurlSetOptException(url, _curlError));
      }
      ZYPP_THROW(MediaWriteException("/dev/null"));
  }

  ret = curl_easy_setopt( _curl, CURLOPT_WRITEDATA, file );
  if ( ret != 0 ) {
      ::fclose(file);
      std::string err( _curlError);
      curl_easy_setopt( _curl, CURLOPT_RANGE, NULL );
      curl_easy_setopt( _curl, CURLOPT_NOBODY, NULL );
      /* yes, this is why we never got to get NOBODY working before,
       because setting it changes this option too, and we also
       need to reset it
       See: http://curl.haxx.se/mail/archive-2005-07/0073.html
      */
      curl_easy_setopt( _curl, CURLOPT_HTTPGET, 1 );
      if ( ret != 0 ) {
          ZYPP_THROW(MediaCurlSetOptException(url, _curlError));
      }
      ZYPP_THROW(MediaCurlSetOptException(url, err));
  }

  CURLcode ok = curl_easy_perform( _curl );
  MIL << "perform code: " << ok << " [ " << curl_easy_strerror(ok) << " ]" << endl;

  // reset curl settings
  if (  _url.getScheme() == "http" ||  _url.getScheme() == "https" )
  {
    curl_easy_setopt( _curl, CURLOPT_NOBODY, NULL );
    if ( ret != 0 ) {
      ZYPP_THROW(MediaCurlSetOptException(url, _curlError));
    }

    /* yes, this is why we never got to get NOBODY working before,
       because setting it changes this option too, and we also
       need to reset it
       See: http://curl.haxx.se/mail/archive-2005-07/0073.html
    */
    curl_easy_setopt( _curl, CURLOPT_HTTPGET, 1);
    if ( ret != 0 ) {
      ZYPP_THROW(MediaCurlSetOptException(url, _curlError));
    }

  }
  else
  {
    // for FTP we set different options
    curl_easy_setopt( _curl, CURLOPT_RANGE, NULL);
    if ( ret != 0 ) {
      ZYPP_THROW(MediaCurlSetOptException(url, _curlError));
    }
  }

  // if the code is not zero, close the file
  if ( ok != 0 )
      ::fclose(file);

  // as we are not having user interaction, the user can't cancel
  // the file existence checking, a callback or timeout return code
  // will be always a timeout.
  try {
      evaluateCurlCode( filename, ok, true /* timeout */);
  }
  catch ( const MediaFileNotFoundException &e ) {
      // if the file did not exist then we can return false
      return false;
  }
  catch ( const MediaException &e ) {
      // some error, we are not sure about file existence, rethrw
      ZYPP_RETHROW(e);
  }
  // exists
  return ( ok == CURLE_OK );
}

///////////////////////////////////////////////////////////////////

void MediaCurl::doGetFileCopy( const Pathname & filename , const Pathname & target, callback::SendReport<DownloadProgressReport> & report, RequestOptions options ) const
{
    DBG << filename.asString() << endl;

    if(!_url.isValid())
      ZYPP_THROW(MediaBadUrlException(_url));

    if(_url.getHost().empty())
      ZYPP_THROW(MediaBadUrlEmptyHostException(_url));

    Url url(getFileUrl(_url, filename));

    Pathname dest = target.absolutename();
    if( assert_dir( dest.dirname() ) )
    {
      DBG << "assert_dir " << dest.dirname() << " failed" << endl;
      ZYPP_THROW( MediaSystemException(url, "System error on " + dest.dirname().asString()) );
    }

    DBG << "URL: " << url.asString() << endl;
    // Use URL without options and without username and passwd
    // (some proxies dislike them in the URL).
    // Curl seems to need the just scheme, hostname and a path;
    // the rest was already passed as curl options (in attachTo).
    Url curlUrl( url );

    // Use asString + url::ViewOptions instead?
    curlUrl.setUsername( "" );
    curlUrl.setPassword( "" );
    curlUrl.setPathParams( "" );
    curlUrl.setQueryString( "" );
    curlUrl.setFragment( "" );

    //
    // See also Bug #154197 and ftp url definition in RFC 1738:
    // The url "ftp://user@host/foo/bar/file" contains a path,
    // that is relative to the user's home.
    // The url "ftp://user@host//foo/bar/file" (or also with
    // encoded slash as %2f) "ftp://user@host/%2ffoo/bar/file"
    // contains an absolute path.
    //
    string urlBuffer( curlUrl.asString());
    CURLcode ret = curl_easy_setopt( _curl, CURLOPT_URL,
                                     urlBuffer.c_str() );
    if ( ret != 0 ) {
      ZYPP_THROW(MediaCurlSetOptException(url, _curlError));
    }

    // set IFMODSINCE time condition (no download if not modified)
    if( PathInfo(target).isExist() )
    {
      curl_easy_setopt(_curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
      curl_easy_setopt(_curl, CURLOPT_TIMEVALUE, PathInfo(target).mtime());
    }
    else
    {
      curl_easy_setopt(_curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_NONE);
      curl_easy_setopt(_curl, CURLOPT_TIMEVALUE, 0);
    }

    string destNew = target.asString() + ".new.zypp.XXXXXX";
    char *buf = ::strdup( destNew.c_str());
    if( !buf)
    {
      ERR << "out of memory for temp file name" << endl;
      ZYPP_THROW(MediaSystemException(
        url, "out of memory for temp file name"
      ));
    }

    int tmp_fd = ::mkstemp( buf );
    if( tmp_fd == -1)
    {
      free( buf);
      ERR << "mkstemp failed for file '" << destNew << "'" << endl;
      ZYPP_THROW(MediaWriteException(destNew));
    }
    destNew = buf;
    free( buf);

    FILE *file = ::fdopen( tmp_fd, "w" );
    if ( !file ) {
      ::close( tmp_fd);
      filesystem::unlink( destNew );
      ERR << "fopen failed for file '" << destNew << "'" << endl;
      ZYPP_THROW(MediaWriteException(destNew));
    }

    DBG << "dest: " << dest << endl;
    DBG << "temp: " << destNew << endl;

    ret = curl_easy_setopt( _curl, CURLOPT_WRITEDATA, file );
    if ( ret != 0 ) {
      ::fclose( file );
      filesystem::unlink( destNew );
      ZYPP_THROW(MediaCurlSetOptException(url, _curlError));
    }

    // Set callback and perform.
    ProgressData progressData(_settings.timeout(), url, &report);
    report->start(url, dest);
    if ( curl_easy_setopt( _curl, CURLOPT_PROGRESSDATA, &progressData ) != 0 ) {
      WAR << "Can't set CURLOPT_PROGRESSDATA: " << _curlError << endl;;
    }

    ret = curl_easy_perform( _curl );

    if ( curl_easy_setopt( _curl, CURLOPT_PROGRESSDATA, NULL ) != 0 ) {
      WAR << "Can't unset CURLOPT_PROGRESSDATA: " << _curlError << endl;;
    }

    if ( ret != 0 )
    {
      ERR << "curl error: " << ret << ": " << _curlError
          << ", temp file size " << PathInfo(destNew).size()
          << " byte." << endl;

      ::fclose( file );
      filesystem::unlink( destNew );

      // the timeout is determined by the progress data object
      // which holds wheter the timeout was reached or not,
      // otherwise it would be a user cancel
      try {
        evaluateCurlCode( filename, ret, progressData.reached);
      }
      catch ( const MediaException &e ) {
        // some error, we are not sure about file existence, rethrw
        ZYPP_RETHROW(e);
      }
    }

#if DETECT_DIR_INDEX
    else
    if(curlUrl.getScheme() == "http" ||
       curlUrl.getScheme() == "https")
    {
      //
      // try to check the effective url and set the not_a_file flag
      // if the url path ends with a "/", what usually means, that
      // we've received a directory index (index.html content).
      //
      // Note: This may be dangerous and break file retrieving in
      //       case of some server redirections ... ?
      //
      bool      not_a_file = false;
      char     *ptr = NULL;
      CURLcode  ret = curl_easy_getinfo( _curl,
                                         CURLINFO_EFFECTIVE_URL,
                                         &ptr);
      if ( ret == CURLE_OK && ptr != NULL)
      {
        try
        {
          Url         eurl( ptr);
          std::string path( eurl.getPathName());
          if( !path.empty() && path != "/" && *path.rbegin() == '/')
          {
            DBG << "Effective url ("
                << eurl
                << ") seems to provide the index of a directory"
                << endl;
            not_a_file = true;
          }
        }
        catch( ... )
        {}
      }

      if( not_a_file)
      {
        ::fclose( file );
        filesystem::unlink( destNew );
        ZYPP_THROW(MediaNotAFileException(_url, filename));
      }
    }
#endif // DETECT_DIR_INDEX

    long httpReturnCode = 0;
    CURLcode infoRet = curl_easy_getinfo(_curl,
                                         CURLINFO_RESPONSE_CODE,
                                         &httpReturnCode);
    bool modified = true;
    if (infoRet == CURLE_OK)
    {
      DBG << "HTTP response: " + str::numstring(httpReturnCode);
      if ( httpReturnCode == 304
           || ( httpReturnCode == 213 && _url.getScheme() == "ftp" ) ) // not modified
      {
        DBG << " Not modified.";
        modified = false;
      }
      DBG << endl;
    }
    else
    {
      WAR << "Could not get the reponse code." << endl;
    }

    if (modified || infoRet != CURLE_OK)
    {
      // apply umask
      if ( ::fchmod( ::fileno(file), filesystem::applyUmaskTo( 0644 ) ) )
      {
        ERR << "Failed to chmod file " << destNew << endl;
      }
      ::fclose( file );

      // move the temp file into dest
      if ( rename( destNew, dest ) != 0 ) {
        ERR << "Rename failed" << endl;
        ZYPP_THROW(MediaWriteException(dest));
      }
    }
    else
    {
      // close and remove the temp file
      ::fclose( file );
      filesystem::unlink( destNew );
    }

    DBG << "done: " << PathInfo(dest) << endl;
}

///////////////////////////////////////////////////////////////////

void MediaCurl::getDir( const Pathname & dirname, bool recurse_r ) const
{
  filesystem::DirContent content;
  getDirInfo( content, dirname, /*dots*/false );

  for ( filesystem::DirContent::const_iterator it = content.begin(); it != content.end(); ++it ) {
      Pathname filename = dirname + it->name;
      int res = 0;

      switch ( it->type ) {
      case filesystem::FT_NOT_AVAIL: // old directory.yast contains no typeinfo at all
      case filesystem::FT_FILE:
        getFile( filename );
        break;
      case filesystem::FT_DIR: // newer directory.yast contain at least directory info
        if ( recurse_r ) {
          getDir( filename, recurse_r );
        } else {
          res = assert_dir( localPath( filename ) );
          if ( res ) {
            WAR << "Ignore error (" << res <<  ") on creating local directory '" << localPath( filename ) << "'" << endl;
          }
        }
        break;
      default:
        // don't provide devices, sockets, etc.
        break;
      }
  }
}

///////////////////////////////////////////////////////////////////

void MediaCurl::getDirInfo( std::list<std::string> & retlist,
                               const Pathname & dirname, bool dots ) const
{
  getDirectoryYast( retlist, dirname, dots );
}

///////////////////////////////////////////////////////////////////

void MediaCurl::getDirInfo( filesystem::DirContent & retlist,
                            const Pathname & dirname, bool dots ) const
{
  getDirectoryYast( retlist, dirname, dots );
}

///////////////////////////////////////////////////////////////////

int MediaCurl::progressCallback( void *clientp,
                                 double dltotal, double dlnow,
                                 double ultotal, double ulnow)
{
  ProgressData *pdata = reinterpret_cast<ProgressData *>(clientp);
  if( pdata)
  {
    time_t now   = time(NULL);
    if( now > 0)
    {
    	// reset time of last change in case initial time()
	// failed or the time was adjusted (goes backward)
	if( pdata->ltime <= 0 || pdata->ltime > now)
	{
	  pdata->ltime = now;
	}

	// start time counting as soon as first data arrives
	// (skip the connection / redirection time at begin)
	time_t dif = 0;
	if (dlnow > 0 || ulnow > 0)
	{
    	  dif = (now - pdata->ltime);
	  dif = dif > 0 ? dif : 0;

	  pdata->secs += dif;
	}

	// update the drate_avg and drate_period only after a second has passed
	// (this callback is called much more often than a second)
	// otherwise the values would be far from accurate when measuring
	// the time in seconds
	//! \todo more accurate download rate computationn, e.g. compute average value from last 5 seconds, or work with milliseconds instead of seconds

        if ( pdata->secs > 1 && (dif > 0 || dlnow == dltotal ))
          pdata->drate_avg = (dlnow / pdata->secs);

	if ( dif > 0 )
	{
	  pdata->drate_period = ((dlnow - pdata->dload_period) / dif);
	  pdata->dload_period = dlnow;
	}
    }

    // send progress report first, abort transfer if requested
    if( pdata->report)
    {
      if (!(*(pdata->report))->progress(int( dlnow * 100 / dltotal ),
	                                pdata->url,
	                                pdata->drate_avg,
	                                pdata->drate_period))
      {
        return 1; // abort transfer
      }
    }

    // check if we there is a timeout set
    if( pdata->timeout > 0)
    {
      if( now > 0)
      {
        bool progress = false;

        // update download data if changed, mark progress
        if( dlnow != pdata->dload)
        {
          progress     = true;
          pdata->dload = dlnow;
          pdata->ltime = now;
        }
        // update upload data if changed, mark progress
        if( ulnow != pdata->uload)
        {
          progress     = true;
          pdata->uload = ulnow;
          pdata->ltime = now;
        }

        if( !progress && (now >= (pdata->ltime + pdata->timeout)))
        {
          pdata->reached = true;
          return 1; // aborts transfer
        }
      }
    }
  }
  return 0;
}

///////////////////////////////////////////////////////////////////

string MediaCurl::getAuthHint() const
{
  long auth_info = CURLAUTH_NONE;

  CURLcode infoRet =
    curl_easy_getinfo(_curl, CURLINFO_HTTPAUTH_AVAIL, &auth_info);

  if(infoRet == CURLE_OK)
  {
    return CurlAuthData::auth_type_long2str(auth_info);
  }

  return "";
}

///////////////////////////////////////////////////////////////////

bool MediaCurl::authenticate(const string & availAuthTypes, bool firstTry) const
{
  //! \todo need a way to pass different CredManagerOptions here
  Target_Ptr target = zypp::getZYpp()->getTarget();
  CredentialManager cm(CredManagerOptions(target ? target->root() : ""));
  CurlAuthData_Ptr credentials;

  // get stored credentials
  AuthData_Ptr cmcred = cm.getCred(_url);

  if (cmcred && firstTry)
  {
    credentials.reset(new CurlAuthData(*cmcred));
    DBG << "got stored credentials:" << endl << *credentials << endl;
  }
  // if not found, ask user
  else
  {

    CurlAuthData_Ptr curlcred;
    curlcred.reset(new CurlAuthData());
    callback::SendReport<AuthenticationReport> auth_report;

    // preset the username if present in current url
    if (!_url.getUsername().empty() && firstTry)
      curlcred->setUsername(_url.getUsername());
    // if CM has found some credentials, preset the username from there
    else if (cmcred)
      curlcred->setUsername(cmcred->username());

    // indicate we have no good credentials from CM
    cmcred.reset();

    string prompt_msg = boost::str(boost::format(
      //!\todo add comma to the message for the next release
      _("Authentication required for '%s'")) % _url.asString());

    // set available authentication types from the exception
    // might be needed in prompt
    curlcred->setAuthType(availAuthTypes);

    // ask user
    if (auth_report->prompt(_url, prompt_msg, *curlcred))
    {
      DBG << "callback answer: retry" << endl
          << "CurlAuthData: " << *curlcred << endl;

      if (curlcred->valid())
      {
        credentials = curlcred;
          // if (credentials->username() != _url.getUsername())
          //   _url.setUsername(credentials->username());
          /**
           *  \todo find a way to save the url with changed username
           *  back to repoinfo or dont store urls with username
           *  (and either forbid more repos with the same url and different
           *  user, or return a set of credentials from CM and try them one
           *  by one)
           */
      }
    }
    else
    {
      DBG << "callback answer: cancel" << endl;
    }
  }

  // set username and password
  if (credentials)
  {
    // HACK, why is this const?
    const_cast<MediaCurl*>(this)->_settings.setUsername(credentials->username());
    const_cast<MediaCurl*>(this)->_settings.setPassword(credentials->password());

    // set username and password
    CURLcode ret = curl_easy_setopt(_curl, CURLOPT_USERPWD, _settings.userPassword().c_str());
    if ( ret != 0 ) ZYPP_THROW(MediaCurlSetOptException(_url, _curlError));

    // set available authentication types from the exception
    if (credentials->authType() == CURLAUTH_NONE)
      credentials->setAuthType(availAuthTypes);

    // set auth type (seems this must be set _after_ setting the userpwd
    if (credentials->authType() != CURLAUTH_NONE)
    {
      ret = curl_easy_setopt(_curl, CURLOPT_HTTPAUTH, credentials->authType());
      if ( ret != 0 ) ZYPP_THROW(MediaCurlSetOptException(_url, _curlError));
    }

    if (!cmcred)
    {
      credentials->setUrl(_url);
      cm.addCred(*credentials);
      cm.save();
    }

    return true;
  }

  return false;
}


  } // namespace media
} // namespace zypp
//
