/*---------------------------------------------------------------------\
|                          ____ _   __ __ ___                          |
|                         |__  / \ / / . \ . \                         |
|                           / / \ V /|  _/  _/                         |
|                          / /__ | | | | | |                           |
|                         /_____||_| |_| |_|                           |
|                                                                      |
\---------------------------------------------------------------------*/
/** \file zypp/capability/CapabilityImpl.h
 *
*/
#ifndef ZYPP_CAPABILITY_CAPABILITYIMPL_H
#define ZYPP_CAPABILITY_CAPABILITYIMPL_H

#include "zypp/base/ReferenceCounted.h"
#include "zypp/base/NonCopyable.h"
#include "zypp/base/KindOf.h"

#include "zypp/Resolvable.h" // maybe ResTraits are sufficient?
#include "zypp/SolverContextFwd.h"

///////////////////////////////////////////////////////////////////
namespace zypp
{ /////////////////////////////////////////////////////////////////
  ///////////////////////////////////////////////////////////////////
  namespace capability
  { /////////////////////////////////////////////////////////////////
    DEFINE_PTR_TYPE(CapabilityImpl)

    ///////////////////////////////////////////////////////////////////
    //
    //	CLASS NAME : CapabilityImpl
    //
    /** Abstract base for Capability implementations. */
    class CapabilityImpl : public base::ReferenceCounted, private base::NonCopyable
    {
    public:
      typedef base::KindOf<Capability> Kind;

    public:
      /** Ctor taking the kind of Resolvable \c this refers to.*/
      CapabilityImpl( const Resolvable::Kind & refers_r );

    public:
      /** Kind of capabiliy.  */
      virtual const Kind & kind() const = 0;

      /** Kind of Resolvable \c this refers to. */
      const Resolvable::Kind & refers() const
      { return _refers; }

      /** More or less human readable representation as string. */
      virtual std::string asString() const = 0;

      /**  */
      virtual bool matches( Resolvable::constPtr resolvable_r,
                            const SolverContext & colverContext_r ) const = 0;

    private:
      /** Kind of Resolvable \c this refers to. */
      Resolvable::Kind _refers;
    };
    ///////////////////////////////////////////////////////////////////

    /** \relates CapabilityImpl Stream output */
    extern std::ostream & operator<<( std::ostream & str, const CapabilityImpl & obj );

    /////////////////////////////////////////////////////////////////
  } // namespace capability
  ///////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////
} // namespace zypp
///////////////////////////////////////////////////////////////////
#endif // ZYPP_CAPABILITY_CAPABILITYIMPL_H
