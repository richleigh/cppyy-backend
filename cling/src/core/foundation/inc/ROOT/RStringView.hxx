// -*- C++ -*-
// Author: Philippe Canal, March 2015

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef RStringView_H
#define RStringView_H

#include "RConfigure.h"

// Don't rely on RConfigure.h to figure whether `string_view` is available, as the
// configuration is fixed at build time, but Cling (or the PCH) can be used with a
// different standard at run-time, e.g. through EXTRA_CLING_ARGS

#if (__cplusplus > 201402L) || (defined(_MSC_VER) && _MSVC_LANG > 201402L)
# ifndef R__HAS_STD_STRING_VIEW
#  define R__HAS_STD_STRING_VIEW
# endif
#else
# ifdef R__HAS_STD_STRING_VIEW
#  undef R__HAS_STD_STRING_VIEW
# endif
#endif

#ifdef R__HAS_STD_STRING_VIEW

#include <string_view>

#else

# if defined(R__HAS_STD_EXPERIMENTAL_STRING_VIEW)
#  include <experimental/string_view>
# else
#  include "RWrap_libcpp_string_view.h"
# endif

namespace std {

   template<class _CharT, class _Traits = std::char_traits<_CharT> >
   using basic_string_view = ::std::experimental::basic_string_view<_CharT,_Traits>;

   // basic_string_view typedef names
   typedef basic_string_view<char> string_view;
   typedef basic_string_view<char16_t> u16string_view;
   typedef basic_string_view<char32_t> u32string_view;
   typedef basic_string_view<wchar_t> wstring_view;

//   template<class _CharT, class _Traits = std::char_traits<_CharT> >
//   basic_string_view<_CharT,_Traits>
//   &operator=(basic_string_view<_CharT,_Traits> &lhs, const TString &rsh) {
//      *lhs = basic_string_view<_CharT,_Traits>(rsh);
//      return *lhs;
//   }

#ifndef R__HAS_STOD_STRING_VIEW
   inline double stod(std::string_view str, size_t *pos)
   {
      return std::stod(std::string(str.data(), str.size()),pos);
   }
#endif

}

#endif // ifdef else __cplusplus > 201402L

#ifndef R__HAS_OP_EQUAL_PLUS_STRING_VIEW

#include <string>

namespace std {
inline namespace __CppyyLegacy {

inline std::string &operator+=(std::string &left, std::string_view right)
{
   return left.append(right.data(), right.size());
}

} // namespace __CppyyLegacy
} // namespace std

#endif // ifndef R__HAS_OP_EQUAL_PLUS_STRING_VIEW

#endif // RStringView_H
