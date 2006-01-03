/*---------------------------------------------------------------------\
|                          ____ _   __ __ ___                          |
|                         |__  / \ / / . \ . \                         |
|                           / / \ V /|  _/  _/                         |
|                          / /__ | | | | | |                           |
|                         /_____||_| |_| |_|                           |
|                                                                      |
\---------------------------------------------------------------------*/
/** \file zypp/target/rpm/RpmHeader.h
 *
*/
#ifndef ZYPP_TARGET_RPM_RPMHEADER_H
#define ZYPP_TARGET_RPM_RPMHEADER_H

#include <iosfwd>
#include <list>

#include "zypp/target/rpm/BinHeader.h"

#include "zypp/Package.h"
#include "zypp/Changelog.h"
#include "zypp/CapSetFwd.h"
#include "zypp/Pathname.h"

namespace zypp {
  namespace target {
    namespace rpm {

#if 0
      class PkgDu;
#endif
      
      ///////////////////////////////////////////////////////////////////
      //
      //	CLASS NAME : RpmHeader
      /**
       * @short Wrapper class for rpm header struct.
       *
       * <code>RpmHeader</code> provides methods to query the content
       * of a rpm header struct retrieved from the RPM database or by reading
       * the rpm header of a package on disk.
       *
       * The rpm header contains all data associated with a package. So you
       * probabely do not want to permanently store too many of them.
       *
       * <B>NEVER create <code>RpmHeader</code> from a NULL <code>Header</code>! </B>
       **/
      class RpmHeader : public BinHeader {
        public:
	  typedef intrusive_ptr<RpmHeader> Ptr;
	  typedef intrusive_ptr<const RpmHeader> constPtr;

        private:
      
          CapSet PkgRelList_val( tag tag_r, bool pre, std::set<std::string> * freq_r = 0 ) const;
      
        public:
      
          /**
           *
           **/
          RpmHeader( Header h_r = 0 );
      
          /**
           * <B>Dangerous!<\B> This one takes the header out of rhs
           * and leaves rhs empty.
           **/
          RpmHeader( BinHeader::Ptr & rhs );
      
          virtual ~RpmHeader();
      
          bool isSrc() const;
      
        public:
      
          std::string    tag_name()        const;
          Edition tag_edition()     const;
          Arch    tag_arch()        const;
      
          Date       tag_installtime() const;
          Date       tag_buildtime()   const;
      
          /**
           * If <code>freq_r</code> is not NULL, file dependencies found are inserted.
           **/
          CapSet tag_provides ( std::set<std::string> * freq_r = 0 ) const;
          /**
           * @see #tag_provides
           **/
          CapSet tag_requires ( std::set<std::string> * freq_r = 0 ) const;
          /**
           * @see #tag_provides
           **/
          CapSet tag_prerequires ( std::set<std::string> * freq_r = 0 ) const;
          /**
           * @see #tag_provides
           **/
          CapSet tag_conflicts( std::set<std::string> * freq_r = 0 ) const;
          /**
           * @see #tag_provides
           **/
          CapSet tag_obsoletes( std::set<std::string> * freq_r = 0 ) const;
      
          ByteCount tag_size()        const;
          ByteCount tag_archivesize() const;
      
          std::string tag_summary()      const;
          std::string tag_description()  const;
          std::string tag_group()        const;
          std::string tag_vendor()       const;
          std::string tag_distribution() const;
          std::string tag_license()      const;
          std::string tag_buildhost()    const;
          std::string tag_packager()     const;
          std::string tag_url()          const;
          std::string tag_os()           const;
          std::string tag_prein()        const;
          std::string tag_postin()       const;
          std::string tag_preun()        const;
          std::string tag_postun()       const;
          std::string tag_sourcerpm()    const;
      
          std::list<std::string> tag_filenames() const;
      
          Changelog tag_changelog() const;
     
#warning FIXME disk usage data
#if 0 
          /**
           * Returns reference to arg <code>dudata_r</code>.
           **/
          PkgDu & tag_du( PkgDu & dudata_r ) const;
#endif
      
        public:
      
          virtual std::ostream & dumpOn( std::ostream & str ) const;
      
        public:
      
          /**
           * Digest and signature verification flags
           **/
          enum VERIFICATION {
            VERIFY       = 0x0000,
            NODIGEST     = (1<<0),
            NOSIGNATURE  = (1<<1),
            NOVERIFY     = 0xffff
          };
      
          /**
           * Get an accessible packages data from disk.
           * Returns NULL on any error.
           **/
          static RpmHeader::constPtr readPackage( const Pathname & path,
      					  VERIFICATION verification = VERIFY );
      };

///////////////////////////////////////////////////////////////////
    } // namespace rpm
  } // namespace target
} // namespace zypp

#endif // ZYPP_TARGET_RPM_RPMHEADER_H

