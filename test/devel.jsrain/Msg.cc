#include <iostream>
#include <zypp/base/Logger.h>
#include <zypp/detail/MessageImpl.h>


using namespace std;
using namespace zypp;

class MyMessageImpl : public detail::MessageImpl
{
  public:
    MyMessageImpl (std::string text)
    : MessageImpl ("message1",
		   Edition (),
		   Arch ("noarch"))
    {
      _text = text;
    }
};

/******************************************************************
**
**
**	FUNCTION NAME : main
**	FUNCTION TYPE : int
**
**	DESCRIPTION :
*/
int main( int argc, char * argv[] )
{
  INT << "===[START]==========================================" << endl;

  MyMessageImpl p ("My message to be shown to user");

  DBG << p << endl;
  DBG << "  \"" << p.text() << "\"" << endl;

  INT << "===[END]============================================" << endl;
  return 0;
}
