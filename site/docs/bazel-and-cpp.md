---
layout: documentation
title: C++ and Bazel
---

# C++ and Bazel

This page contains resources that help you use Bazel with C++ projects. It links
to a tutorial, build rules, and other information specific to building C++
projects with Bazel.

## Working with Bazel

The following resources will help you work with Bazel on C++ projects:

*  [Tutorial: Building a C++ project](tutorial/cpp.html)
*  [C++ common use cases](cpp-use-cases.html)
*  [C/C++ rules](be/c-cpp.html)
*  [C++ toolchain configuration](cc-toolchain-config-reference.html)
*  [Tutorial: Configuring C++ toolchains](tutorial/cc-toolchain-config.html)
*  [Integrating with C++ rules](integrating-with-rules-cc.html)

## Best practices

In addition to [general Bazel best practices](best-practices.html), below are
best practices specific to C++ projects.

### BUILD files

Follow the guidelines below when creating your BUILD files:

*  Each `BUILD` file should contain one [`cc_library`](be/c-cpp.html#cc_library)
   rule target per compilation unit in the directory.

*  You should granularize your C++ libraries as much as
   possible to maximize incrementality and parallelize the build.

*  If there is a single source file in `srcs`, name the library the same as
   that C++ file's name. This library should contain C++ file(s), any matching
   header file(s), and the library's direct dependencies. For example:

   ```python
   cc_library(
       name = "mylib",
       srcs = ["mylib.cc"],
       hdrs = ["mylib.h"],
       deps = [":lower-level-lib"]
   )
   ```

*  Use one `cc_test` rule target per `cc_library` target in the file. Name the
   target `[library-name]_test` and the source file `[library-name]_test.cc`.
   For example, a test target for the `mylib` library target shown above would
   look like this:

   ```python
   cc_test(
       name = "mylib_test",
       srcs = ["mylib_test.cc"],
       deps = [":mylib"]
   )
   ```

### Include paths

Follow these guidelines for include paths:

*  Make all include paths relative to the workspace directory.

*  Use quoted includes (`#include "foo/bar/baz.h"`) for non-system headers, not
   angle-brackets (`#include <foo/bar/baz.h>`).

*  Avoid using UNIX directory shortcuts, such as `.` (current directory) or `..`
   (parent directory).

*  For legacy or `third_party` code that requires includes pointing outside the
   project repository, such as external repository includes requiring a prefix,
   use the [`include_prefix`](be/c-cpp.html#cc_library.include_prefix) and
   [`strip_include_prefix`](be/c-cpp.html#cc_library.strip_include_prefix)
   arguments on the `cc_library` rule target.


## Include Scanning

### Basic Usage
Enables the use of C/C++ include scanning when the ```--experimental_include_scanning``` flag is provided to the ```bazel build``` command. This flag improves performance by decreasing the size of compilation input trees.

### Use cases
--experimental_include_scanning is suited for the building of projects in C/C++ where the overhead of header files is large and it is not clear whether or not all of the header files are necessary for the compilation.

### Limitations
  - ```experimental_include_scanning``` does not use preprocessor directives, and so the flag cannot be used on projects which include them.
  - Include scanning must not be enabled by default because limitations of the builtin include parser can break builds.
  - There can be missing inlcudes when building a project with ```experimental_inlcude_scanning```, these can be overcome with the use of either patch files or Include Hints.

### Advanced Features
  - Include Hints:
    - Include Hints are a hidden feature within the inlcude scanning patch, this consists of creating the file ```tools/cpp/INCLUDE_HINTS```. The INLCUDE_HINTS file contains regexp-based rules to help experimental_include_scanning cope with computed includes, which would otherwise require a full preprocessor with symbol support. Each line of the file should be split into four columns: ```&quot;file&quot;|&quot;path&quot;  match-pattern  find-root  find-filter```.
      - Column 1: The first column specifies whether the line is a rule based on matching source **files** (passed directly to the compiler as inputs, or transitively #included by other inputs) or include **paths**.
      - Column 2: The second column is a regexp for files or paths. Whenever a compiler argument of the specified type matches that regexp, the rule is taken.
      - Column 3: The third column is a point in the local filesystem from which to extract a recursive listing.
      - Column 4: The fourth column is a regexp applied to each file found by the recursive listing. All  matching files are treated as dependencies.
  - Patch Files:
    - Patch files can be used to manually add the missing include files if errors with these files are encountered. These patches should affect the ```cc_library``` within the corresponding ```BUILD.bazel``` file. This could consist of removing specific files from the exclude list or adding the file into the include list.
