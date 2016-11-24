//===- FormatVariadic.h - Efficient type-safe string formatting --*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the formatv() function which can be used with other LLVM
// subsystems to provide printf-like formatting, but with improved safety and
// flexibility.  The result of `formatv` is an object which can be streamed to
// a raw_ostream or converted to a std::string or llvm::SmallString.
//
//   // Convert to std::string.
//   std::string S = formatv("{0} {1}", 1234.412, "test").str();
//
//   // Convert to llvm::SmallString
//   SmallString<8> S = formatv("{0} {1}", 1234.412, "test").sstr<8>();
//
//   // Stream to an existing raw_ostream.
//   OS << formatv("{0} {1}", 1234.412, "test");
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_FORMATVARIADIC_H
#define LLVM_SUPPORT_FORMATVARIADIC_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/FormatCommon.h"
#include "llvm/Support/FormatProviders.h"
#include "llvm/Support/FormatVariadicDetails.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <tuple>
#include <vector>

namespace llvm {

enum class ReplacementType { Empty, Format, Literal };

struct ReplacementItem {
  ReplacementItem() {}
  explicit ReplacementItem(StringRef Literal)
      : Type(ReplacementType::Literal), Spec(Literal) {}
  ReplacementItem(StringRef Spec, size_t Index, size_t Align, AlignStyle Where,
                  char Pad, StringRef Options)
      : Type(ReplacementType::Format), Spec(Spec), Index(Index), Align(Align),
        Where(Where), Pad(Pad), Options(Options) {}
  ReplacementType Type = ReplacementType::Empty;
  StringRef Spec;
  size_t Index = 0;
  size_t Align = 0;
  AlignStyle Where = AlignStyle::Right;
  char Pad;
  StringRef Options;
};

class formatv_object_base {
protected:
  // The parameters are stored in a std::tuple, which does not provide runtime
  // indexing capabilities.  In order to enable runtime indexing, we use this
  // structure to put the parameters into a std::vector.  Since the parameters
  // are not all the same type, we use some type-erasure by wrapping the
  // parameters in a template class that derives from a non-template superclass.
  // Essentially, we are converting a std::tuple<Derived<Ts...>> to a
  // std::vector<Base*>.
  struct create_wrappers {
    template <typename... Ts>
    std::vector<detail::format_wrapper *> operator()(Ts &... Items) {
      return std::vector<detail::format_wrapper *>{&Items...};
    }
  };

  StringRef Fmt;
  std::vector<detail::format_wrapper *> Wrappers;
  std::vector<ReplacementItem> Replacements;

  static bool consumeFieldLayout(StringRef &Spec, AlignStyle &Where,
                                 size_t &Align, char &Pad);

  static std::pair<ReplacementItem, StringRef>
  splitLiteralAndReplacement(StringRef Fmt);

public:
  formatv_object_base(StringRef Fmt, std::size_t ParamCount)
      : Fmt(Fmt), Replacements(parseFormatString(Fmt)) {
    Wrappers.reserve(ParamCount);
    return;
  }

  void format(raw_ostream &S) const {
    for (auto &R : Replacements) {
      if (R.Type == ReplacementType::Empty)
        continue;
      if (R.Type == ReplacementType::Literal) {
        S << R.Spec;
        continue;
      }
      if (R.Index >= Wrappers.size()) {
        S << R.Spec;
        continue;
      }

      auto W = Wrappers[R.Index];

      FmtAlign Align(*W, R.Where, R.Align);
      Align.format(S, R.Options);
    }
  }
  static std::vector<ReplacementItem> parseFormatString(StringRef Fmt);

  static Optional<ReplacementItem> parseReplacementItem(StringRef Spec);

  std::string str() const {
    std::string Result;
    raw_string_ostream Stream(Result);
    Stream << *this;
    Stream.flush();
    return Result;
  }

  template <unsigned N> llvm::SmallString<N> sstr() const {
    SmallString<N> Result;
    raw_svector_ostream Stream(Result);
    Stream << *this;
    return Result;
  }

  template <unsigned N> operator SmallString<N>() const { return sstr<N>(); }

  operator std::string() const { return str(); }
};

template <typename Tuple> class formatv_object : public formatv_object_base {
  // Storage for the parameter wrappers.  Since the base class erases the type
  // of the parameters, we have to own the storage for the parameters here, and
  // have the base class store type-erased pointers into this tuple.
  Tuple Parameters;

public:
  formatv_object(StringRef Fmt, Tuple &&Params)
      : formatv_object_base(Fmt, std::tuple_size<Tuple>::value),
        Parameters(std::move(Params)) {
    Wrappers = apply_tuple(create_wrappers(), Parameters);
  }
};

// \brief Format text given a format string and replacement parameters.
//
// ===General Description===
//
// Formats textual output.  `Fmt` is a string consisting of one or more
// replacement sequences with the following grammar:
//
// rep_field ::= "{" [index] ["," layout] [":" format] "}"
// index     ::= <non-negative integer>
// layout    ::= [[[char]loc]width]
// format    ::= <any string not containing "{" or "}">
// char      ::= <any character except "{" or "}">
// loc       ::= "-" | "=" | "+"
// width     ::= <positive integer>
//
// index   - A non-negative integer specifying the index of the item in the
//           parameter pack to print.  Any other value is invalid.
// layout  - A string controlling how the field is laid out within the available
//           space.
// format  - A type-dependent string used to provide additional options to
//           the formatting operation.  Refer to the documentation of the
//           various individual format providers for per-type options.
// char    - The padding character.  Defaults to ' ' (space).  Only valid if
//           `loc` is also specified.
// loc     - Where to print the formatted text within the field.  Only valid if
//           `width` is also specified.
//           '-' : The field is left aligned within the available space.
//           '=' : The field is centered within the available space.
//           '+' : The field is right aligned within the available space (this
//                 is the default).
// width   - The width of the field within which to print the formatted text.
//           If this is less than the required length then the `char` and `loc`
//           fields are ignored, and the field is printed with no leading or
//           trailing padding.  If this is greater than the required length,
//           then the text is output according to the value of `loc`, and padded
//           as appropriate on the left and/or right by `char`.
//
// ===Special Characters===
//
// The characters '{' and '}' are reserved and cannot appear anywhere within a
// replacement sequence.  Outside of a replacement sequence, in order to print
// a literal '{' or '}' it must be doubled -- "{{" to print a literal '{' and
// "}}" to print a literal '}'.
//
// ===Parameter Indexing===
// `index` specifies the index of the paramter in the parameter pack to format
// into the output.  Note that it is possible to refer to the same parameter
// index multiple times in a given format string.  This makes it possible to
// output the same value multiple times without passing it multiple times to the
// function. For example:
//
//   formatv("{0} {1} {0}", "a", "bb")
//
// would yield the string "abba".  This can be convenient when it is expensive
// to compute the value of the parameter, and you would otherwise have had to
// save it to a temporary.
//
// ===Formatter Search===
//
// For a given parameter of type T, the following steps are executed in order
// until a match is found:
//
//   1. If the parameter is of class type, and contains a method
//      void format(raw_ostream &Stream, StringRef Options)
//      Then this method is invoked to produce the formatted output.  The
//      implementation should write the formatted text into `Stream`.
//   2. If there is a suitable template specialization of format_provider<>
//      for type T containing a method whose signature is:
//      void format(const T &Obj, raw_ostream &Stream, StringRef Options)
//      Then this method is invoked as described in Step 1.
//
// If a match cannot be found through either of the above methods, a compiler
// error is generated.
//
// ===Invalid Format String Handling===
//
// In the case of a format string which does not match the grammar described
// above, the output is undefined.  With asserts enabled, LLVM will trigger an
// assertion.  Otherwise, it will try to do something reasonable, but in general
// the details of what that is are undefined.
//
template <typename... Ts>
inline auto formatv(const char *Fmt, Ts &&... Vals) -> formatv_object<decltype(
    std::make_tuple(detail::build_format_wrapper(std::forward<Ts>(Vals))...))> {
  using ParamTuple = decltype(
      std::make_tuple(detail::build_format_wrapper(std::forward<Ts>(Vals))...));
  return formatv_object<ParamTuple>(
      Fmt,
      std::make_tuple(detail::build_format_wrapper(std::forward<Ts>(Vals))...));
}

} // end namespace llvm

#endif
