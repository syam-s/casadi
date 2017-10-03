/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */



#include "code_generator.hpp"
#include "function_internal.hpp"
#include <iomanip>
#include <casadi_runtime_str.h>

using namespace std;
namespace casadi {

  CodeGenerator::CodeGenerator(const std::string& name, const Dict& opts) {
    // Default options
    this->verbose = true;
    this->mex = false;
    this->cpp = false;
    this->main = false;
    this->casadi_real = "double";
    this->codegen_scalars = false;
    this->with_header = false;
    this->with_mem = false;
    this->with_export = true;
    indent_ = 2;

    // Read options
    for (auto&& e : opts) {
      if (e.first=="verbose") {
        this->verbose = e.second;
      } else if (e.first=="mex") {
        this->mex = e.second;
      } else if (e.first=="cpp") {
        this->cpp = e.second;
      } else if (e.first=="main") {
        this->main = e.second;
      } else if (e.first=="casadi_real") {
        this->casadi_real = e.second.to_string();
      } else if (e.first=="codegen_scalars") {
        this->codegen_scalars = e.second;
      } else if (e.first=="with_header") {
        this->with_header = e.second;
      } else if (e.first=="with_mem") {
        this->with_mem = e.second;
      } else if (e.first=="with_export") {
        this->with_export = e.second;
      } else if (e.first=="indent") {
        indent_ = e.second;
        casadi_assert_dev(indent_>=0);
      } else {
        casadi_error("Unrecongnized option: " + str(e.first));
      }
    }

    // Start at new line with no indentation
    newline_ = true;
    current_indent_ = 0;

    // Divide name into base and suffix (if any)
    string::size_type dotpos = name.rfind('.');
    if (dotpos==string::npos) {
      this->name = name;
      this->suffix = this->cpp ? ".cpp" : ".c";
    } else {
      this->name = name.substr(0, dotpos);
      this->suffix = name.substr(dotpos);
    }

    // Symbol prefix
    if (this->with_export) {
      dll_export = "CASADI_SYMBOL_EXPORT ";
    }

    // Make sure that the base name is sane
    casadi_assert_dev(Function::check_name(this->name));

    // Includes needed
    add_include("math.h");
    if (this->main) add_include("stdio.h");

    // Mex and main need string.h
    if (this->mex || this->main) {
      add_include("string.h");
    }

    // Memory struct entry point
    if (this->with_mem) {
      add_include("casadi/mem.h", false);
      this->header << "#include <casadi/mem.h>\n";
    }

    // Mex
    if (this->mex) {
      add_include("mex.h", false, "MATLAB_MEX_FILE");
    }
  }

  std::string CodeGenerator::add_dependency(const Function& f) {
    // Quick return if it already exists
    for (auto&& e : added_functions_) if (e.f==f) return e.codegen_name;

    // Give it a name
    string fname = shorthand("f" + str(added_functions_.size()));

    // Add to list of functions
    added_functions_.push_back({f, fname});

    // Generate declarations
    f->codegen_declarations(*this);

    // Print to file
    f->codegen(*this, fname);

    // Codegen reference count functions, if needed
    if (f->has_refcount_) {
      // Increase reference counter
      *this << "void " << fname << "_incref(void) {\n";
      f->codegen_incref(*this);
      *this << "}\n\n";

      // Decrease reference counter
      *this << "void " << fname << "_decref(void) {\n";
      f->codegen_decref(*this);
      *this << "}\n\n";
    }

    // Flush to body
    flush(this->body);

    return fname;
  }

    void CodeGenerator::add(const Function& f, bool with_jac_sparsity) {
    // Add if not already added
    std::string codegen_name = add_dependency(f);

    // Define function
    *this << declare(f->signature(f.name())) << "{\n"
          << "return " << codegen_name <<  "(arg, res, iw, w, mem);\n"
          << "}\n\n";

    // Generate meta information
    f->codegen_meta(*this);

    // Generate Jacobian sparsity information
    if (with_jac_sparsity) {
      // Generate/get Jacobian sparsity
      Sparsity jac = f->get_jacobian_sparsity();
      // Code generate the sparsity pattern
      add_io_sparsities("jac_" + f.name(), f->sparsity_in_, {jac});

      // Flush buffers
      flush(this->body);
    }

    // Add to list of exposed symbols
    this->exposed_fname.push_back(f.name());
  }

  std::string CodeGenerator::dump() const {
    stringstream s;
    dump(s);
    return s.str();
  }

  void CodeGenerator::file_open(std::ofstream& f, const std::string& name) const {
    // Open a file for writing
    f.open(name);

    // Print header
    f << "/* This file was automatically generated by CasADi.\n"
      << "   The CasADi copyright holders make no ownership claim of its contents. */\n";

    // C linkage
    if (!this->cpp) {
      f << "#ifdef __cplusplus\n"
        << "extern \"C\" {\n"
        << "#endif\n\n";
    }
  }

  void CodeGenerator::file_close(std::ofstream& f) const {
    // C linkage
    if (!this->cpp) {
      f << "#ifdef __cplusplus\n"
        << "} /* extern \"C\" */\n"
        << "#endif\n";
    }

    // Close file(s)
    f.close();
  }

  void CodeGenerator::generate_casadi_real(std::ostream &s) const {
    s << "#ifndef casadi_real\n"
      << "#define casadi_real " << this->casadi_real << endl
      << "#endif\n\n";
  }

  std::string CodeGenerator::generate(const std::string& prefix) const {
    // Throw an error if the prefix contains the filename, since since syntax
    // has changed
    casadi_assert(prefix.find(this->name + this->suffix)==string::npos,
       "The signature of CodeGenerator::generate has changed. "
       "Instead of providing the filename, only provide the prefix.");

    // Create c file
    ofstream s;
    string fullname = prefix + this->name + this->suffix;
    file_open(s, fullname);

    // Dump code to file
    dump(s);

    // Mex entry point
    if (this->mex) generate_mex(s);

    // Main entry point
    if (this->main) generate_main(s);

    // Finalize file
    file_close(s);

    // Generate header
    if (this->with_header) {
      // Create a header file
      file_open(s, prefix + this->name + ".h");

      // Define the casadi_real type (typically double)
      generate_casadi_real(s);

      // Add declarations
      s << this->header.str();

      // Finalize file
      file_close(s);
    }
    return fullname;
  }

  void CodeGenerator::generate_mex(std::ostream &s) const {
    // Begin conditional compilation
    s << "#ifdef MATLAB_MEX_FILE\n";

    // Function prototype
    if (this->cpp) s << "extern \"C\"\n"; // C linkage
    s << "void mexFunction(int resc, mxArray *resv[], int argc, const mxArray *argv[]) {"
      << endl;

    // Create a buffer
    size_t buf_len = 0;
    for (int i=0; i<exposed_fname.size(); ++i) {
      buf_len = std::max(buf_len, exposed_fname[i].size());
    }
    s << "  char buf[" << (buf_len+1) << "];\n";

    // Read string argument
    s << "  int buf_ok = --argc >= 0 && !mxGetString(*argv++, buf, sizeof(buf));\n";

    // Create switch
    s << "  if (!buf_ok) {\n"
      << "    /* name error */\n";
    for (int i=0; i<exposed_fname.size(); ++i) {
      s << "  } else if (strcmp(buf, \"" << exposed_fname[i] << "\")==0) {\n"
        << "    return mex_" << exposed_fname[i] << "(resc, resv, argc, argv);\n";
    }
    s << "  }\n";

    // Error
    s << "  mexErrMsgTxt(\"First input should be a command string. Possible values:";
    for (int i=0; i<exposed_fname.size(); ++i) {
      s << " '" << exposed_fname[i] << "'";
    }
    s << "\");\n";

    // End conditional compilation and function
    s << "}\n"
         << "#endif\n";
  }

  void CodeGenerator::generate_main(std::ostream &s) const {
    s << "int main(int argc, char* argv[]) {\n";

    // Create switch
    s << "  if (argc<2) {\n"
      << "    /* name error */\n";
    for (int i=0; i<exposed_fname.size(); ++i) {
      s << "  } else if (strcmp(argv[1], \"" << exposed_fname[i] << "\")==0) {\n"
        << "    return main_" << exposed_fname[i] << "(argc-2, argv+2);\n";
    }
    s << "  }\n";

    // Error
    s << "  fprintf(stderr, \"First input should be a command string. Possible values:";
    for (int i=0; i<exposed_fname.size(); ++i) {
      s << " '" << exposed_fname[i] << "'";
    }
    s << "\\n\");\n";

    // End main
    s << "  return 1;\n"
      << "}\n";
  }

  void CodeGenerator::dump(std::ostream& s) const {
    // Consistency check
    casadi_assert_dev(current_indent_ == 0);

    // Prefix internal symbols to avoid symbol collisions
    s << "/* How to prefix internal symbols */\n"
      << "#ifdef CODEGEN_PREFIX\n"
      << "  #define NAMESPACE_CONCAT(NS, ID) _NAMESPACE_CONCAT(NS, ID)\n"
      << "  #define _NAMESPACE_CONCAT(NS, ID) NS ## ID\n"
      << "  #define CASADI_PREFIX(ID) NAMESPACE_CONCAT(CODEGEN_PREFIX, ID)\n"
      << "#else\n"
      << "  #define CASADI_PREFIX(ID) " << this->name << "_ ## ID\n"
      << "#endif\n\n";

    s << this->includes.str();
    s << endl;

    // Real type (usually double)
    generate_casadi_real(s);

    // Type conversion
    s << "#define to_double(x) "
      << (this->cpp ? "static_cast<double>(x)" : "(double) x") << endl
      << "#define to_int(x) "
      << (this->cpp ? "static_cast<int>(x)" : "(int) x") << endl
      << "#define CASADI_CAST(x,y) "
      << (this->cpp ? "static_cast<x>(y)" : "(x) y") << endl << endl;

    // Pre-C99
    s << "/* Pre-c99 compatibility */\n"
      << "#if __STDC_VERSION__ < 199901L\n"
      << "  #define fmin CASADI_PREFIX(fmin)\n"
      << "  casadi_real fmin(casadi_real x, casadi_real y) { return x<y ? x : y;}\n"
      << "  #define fmax CASADI_PREFIX(fmax)\n"
      << "  casadi_real fmax(casadi_real x, casadi_real y) { return x>y ? x : y;}\n"
      << "#endif\n\n";

      // CasADi extensions
      s << "/* CasADi extensions */\n"
        << "#define sq CASADI_PREFIX(sq)\n"
        << "casadi_real sq(casadi_real x) { return x*x;}\n"
        << "#define sign CASADI_PREFIX(sign)\n"
        << "casadi_real CASADI_PREFIX(sign)(casadi_real x) { return x<0 ? -1 : x>0 ? 1 : x;}\n"
        << "#define twice CASADI_PREFIX(twice)\n"
        << "casadi_real twice(casadi_real x) { return x+x;}\n\n";

    // Macros
    if (!added_shorthands_.empty()) {
      s << "/* Add prefix to internal symbols */\n";
      for (auto&& i : added_shorthands_) {
        s << "#define " << "casadi_" << i <<  " CASADI_PREFIX(" << i <<  ")\n";
      }
      s << endl;
    }

    // Printing routing
    s << "/* Printing routine */\n";
    if (this->mex) {
      s << "#ifdef MATLAB_MEX_FILE\n"
        << "  #define PRINTF mexPrintf\n"
        << "#else\n"
        << "  #define PRINTF printf\n"
        << "#endif\n";
    } else {
      s << "#define PRINTF printf\n";
    }
    s << endl;

    if (this->with_export) {
      s << "/* Symbol visibility in DLLs */\n"
        << "#ifndef CASADI_SYMBOL_EXPORT\n"
        << "  #if defined(_WIN32) || defined(__WIN32__) || defined(__CYGWIN__)\n"
        << "    #if defined(STATIC_LINKED)\n"
        << "      #define CASADI_SYMBOL_EXPORT\n"
        << "    #else\n"
        << "      #define CASADI_SYMBOL_EXPORT __declspec(dllexport)\n"
        << "    #endif\n"
        << "  #elif defined(__GNUC__) && defined(GCC_HASCLASSVISIBILITY)\n"
        << "    #define CASADI_SYMBOL_EXPORT __attribute__ ((visibility (\"default\")))\n"
        << "  #else"  << endl
        << "    #define CASADI_SYMBOL_EXPORT\n"
        << "  #endif\n"
        << "#endif\n\n";
    }

    // Print integer constants
    if (!integer_constants_.empty()) {
      for (int i=0; i<integer_constants_.size(); ++i) {
        print_vector(s, "casadi_s" + str(i), integer_constants_[i]);
      }
      s << endl;
    }

    // Print double constants
    if (!double_constants_.empty()) {
      for (int i=0; i<double_constants_.size(); ++i) {
        print_vector(s, "casadi_c" + str(i), double_constants_[i]);
      }
      s << endl;
    }

    // External function declarations
    if (!added_externals_.empty()) {
      s << "/* External functions */\n";
      for (auto&& i : added_externals_) {
        s << i << endl;
      }
      s << endl << endl;
    }

    // Codegen auxiliary functions
    s << this->auxiliaries.str();

    // Codegen body
    s << this->body.str();

    // End with new line
    s << endl;
  }

  std::string CodeGenerator::work(int n, int sz) const {
    if (n<0 || sz==0) {
      return "0";
    } else if (sz==1 && !this->codegen_scalars) {
      return "(&w" + str(n) + ")";
    } else {
      return "w" + str(n);
    }
  }

  std::string CodeGenerator::workel(int n) const {
    if (n<0) return "0";
    stringstream s;
    if (this->codegen_scalars) s << "*";
    s << "w" << n;
    return s.str();
  }

  std::string CodeGenerator::array(const std::string& type, const std::string& name, int len,
                                   const std::string& def) {
    std::stringstream s;
    s << type << " ";
    if (len==0) {
      s << "*" << name << " = 0";
    } else {
      s << name << "[" << len << "]";
      if (!def.empty()) s << " = " << def;
    }
    s << ";\n";
    return s.str();
  }

  void CodeGenerator::print_vector(std::ostream &s, const std::string& name, const vector<int>& v) {
    s << array("static const int", name, v.size(), initializer(v));
  }

  void CodeGenerator::print_vector(std::ostream &s, const std::string& name,
                                  const vector<double>& v) {
    s << array("static const casadi_real", name, v.size(), initializer(v));
  }

  void CodeGenerator::add_include(const std::string& new_include, bool relative_path,
                                 const std::string& use_ifdef) {
    // Register the new element
    bool added = added_includes_.insert(new_include).second;

    // Quick return if it already exists
    if (!added) return;

    // Ifdef opening
    if (!use_ifdef.empty()) this->includes << "#ifdef " << use_ifdef << endl;

    // Print to the header section
    if (relative_path) {
      this->includes << "#include \"" << new_include << "\"\n";
    } else {
      this->includes << "#include <" << new_include << ">\n";
    }

    // Ifdef closing
    if (!use_ifdef.empty()) this->includes << "#endif\n";
  }

  std::string CodeGenerator::
  operator()(const Function& f, const std::string& arg,
             const std::string& res, const std::string& iw,
             const std::string& w, const std::string& mem) const {
    return f->codegen_name(*this) + "(" + arg + ", " + res + ", "
      + iw + ", " + w + ", " + mem + ")";
  }

  void CodeGenerator::add_external(const std::string& new_external) {
    added_externals_.insert(new_external);
  }

  std::string CodeGenerator::shorthand(const std::string& name) const {
    casadi_assert(added_shorthands_.count(name), "No such macro: " + name);
    return "casadi_" + name;
  }

  std::string CodeGenerator::shorthand(const std::string& name, bool allow_adding) {
    bool added = added_shorthands_.insert(name).second;
    if (!allow_adding) {
      casadi_assert(added, "Duplicate macro: " + name);
    }
    return "casadi_" + name;
  }

  int CodeGenerator::add_sparsity(const Sparsity& sp) {
    return get_constant(sp, true);
  }

  std::string CodeGenerator::sparsity(const Sparsity& sp) {
    return shorthand("s" + str(add_sparsity(sp)));
  }

  int CodeGenerator::get_sparsity(const Sparsity& sp) const {
    return const_cast<CodeGenerator&>(*this).get_constant(sp, false);
  }

  size_t CodeGenerator::hash(const std::vector<double>& v) {
    // Calculate a hash value for the vector
    std::size_t seed=0;
    if (!v.empty()) {
      casadi_assert_dev(sizeof(double) % sizeof(size_t)==0);
      const int int_len = v.size()*(sizeof(double)/sizeof(size_t));
      const size_t* int_v = reinterpret_cast<const size_t*>(&v.front());
      for (size_t i=0; i<int_len; ++i) {
        hash_combine(seed, int_v[i]);
      }
    }
    return seed;
  }

  size_t CodeGenerator::hash(const std::vector<int>& v) {
    size_t seed=0;
    hash_combine(seed, v);
    return seed;
  }

  int CodeGenerator::get_constant(const std::vector<double>& v, bool allow_adding) {
    // Hash the vector
    size_t h = hash(v);

    // Try to locate it in already added constants
    auto eq = added_double_constants_.equal_range(h);
    for (auto i=eq.first; i!=eq.second; ++i) {
      if (equal(v, double_constants_[i->second])) return i->second;
    }

    if (allow_adding) {
      // Add to constants
      int ind = double_constants_.size();
      double_constants_.push_back(v);
      added_double_constants_.insert(make_pair(h, ind));
      return ind;
    } else {
      casadi_error("Constant not found");
      return -1;
    }
  }

  int CodeGenerator::get_constant(const std::vector<int>& v, bool allow_adding) {
    // Hash the vector
    size_t h = hash(v);

    // Try to locate it in already added constants
    pair<multimap<size_t, size_t>::iterator, multimap<size_t, size_t>::iterator> eq =
      added_integer_constants_.equal_range(h);
    for (multimap<size_t, size_t>::iterator i=eq.first; i!=eq.second; ++i) {
      if (equal(v, integer_constants_[i->second])) return i->second;
    }

    if (allow_adding) {
      // Add to constants
      int ind = integer_constants_.size();
      integer_constants_.push_back(v);
      added_integer_constants_.insert(pair<size_t, size_t>(h, ind));
      return ind;
    } else {
      casadi_error("Constant not found");
      return -1;
    }
  }

  std::string CodeGenerator::constant(const std::vector<int>& v) {
    return shorthand("s" + str(get_constant(v, true)));
  }

  std::string CodeGenerator::constant(const std::vector<double>& v) {
    return shorthand("c" + str(get_constant(v, true)));
  }

  void CodeGenerator::add_auxiliary(Auxiliary f, const vector<string>& inst) {
    // Look for existing instantiations
    auto f_match = added_auxiliaries_.equal_range(f);
    // Look for duplicates
    for (auto it=f_match.first; it!=f_match.second; ++it) {
      if (it->second==inst) return;
    }
    added_auxiliaries_.insert(make_pair(f, inst));

    // Add the appropriate function
    switch (f) {
    case AUX_COPY:
      this->auxiliaries << sanitize_source(casadi_copy_str, inst);
      break;
    case AUX_SWAP:
      this->auxiliaries << sanitize_source(casadi_swap_str, inst);
      break;
    case AUX_SCAL:
      this->auxiliaries << sanitize_source(casadi_scal_str, inst);
      break;
    case AUX_AXPY:
      this->auxiliaries << sanitize_source(casadi_axpy_str, inst);
      break;
    case AUX_DOT:
      this->auxiliaries << sanitize_source(casadi_dot_str, inst);
      break;
    case AUX_BILIN:
      this->auxiliaries << sanitize_source(casadi_bilin_str, inst);
      break;
    case AUX_RANK1:
      this->auxiliaries << sanitize_source(casadi_rank1_str, inst);
      break;
    case AUX_IAMAX:
      this->auxiliaries << sanitize_source(casadi_iamax_str, inst);
      break;
    case AUX_INTERPN:
      add_auxiliary(AUX_INTERPN_WEIGHTS);
      add_auxiliary(AUX_INTERPN_INTERPOLATE);
      add_auxiliary(AUX_FLIP, {});
      add_auxiliary(AUX_FILL);
      add_auxiliary(AUX_FILL, {"int"});
      this->auxiliaries << sanitize_source(casadi_interpn_str, inst);
      break;
    case AUX_INTERPN_GRAD:
      add_auxiliary(AUX_INTERPN);
      this->auxiliaries << sanitize_source(casadi_interpn_grad_str, inst);
      break;
    case AUX_DE_BOOR:
      this->auxiliaries << sanitize_source(casadi_de_boor_str, inst);
      break;
    case AUX_ND_BOOR_EVAL:
      add_auxiliary(AUX_DE_BOOR);
      add_auxiliary(AUX_FILL);
      add_auxiliary(AUX_FILL, {"int"});
      add_auxiliary(AUX_LOW);
      this->auxiliaries << sanitize_source(casadi_nd_boor_eval_str, inst);
      break;
    case AUX_FLIP:
      this->auxiliaries << sanitize_source(casadi_flip_str, inst);
      break;
    case AUX_LOW:
      this->auxiliaries << sanitize_source(casadi_low_str, inst);
      break;
    case AUX_INTERPN_WEIGHTS:
      add_auxiliary(AUX_LOW);
      this->auxiliaries << sanitize_source(casadi_interpn_weights_str, inst);
      break;
    case AUX_INTERPN_INTERPOLATE:
      this->auxiliaries << sanitize_source(casadi_interpn_interpolate_str, inst);
      break;
    case AUX_NORM_1:
      this->auxiliaries << sanitize_source(casadi_norm_1_str, inst);
      break;
    case AUX_NORM_2:
      this->auxiliaries << sanitize_source(casadi_norm_2_str, inst);
      break;
    case AUX_NORM_INF:
      this->auxiliaries << sanitize_source(casadi_norm_inf_str, inst);
      break;
    case AUX_FILL:
      this->auxiliaries << sanitize_source(casadi_fill_str, inst);
      break;
    case AUX_MV:
      this->auxiliaries << sanitize_source(casadi_mv_str, inst);
      break;
    case AUX_MV_DENSE:
      this->auxiliaries << sanitize_source(casadi_mv_dense_str, inst);
      break;
    case AUX_MTIMES:
      this->auxiliaries << sanitize_source(casadi_mtimes_str, inst);
      break;
    case AUX_PROJECT:
      this->auxiliaries << sanitize_source(casadi_project_str, inst);
      break;
    case AUX_DENSIFY:
      add_auxiliary(AUX_FILL);
      {
        std::vector<std::string> inst2 = inst;
        if (inst.size()==1) inst2.push_back(inst[0]);
        this->auxiliaries << sanitize_source(casadi_densify_str, inst2);
      }
      break;
    case AUX_TRANS:
      this->auxiliaries << sanitize_source(casadi_trans_str, inst);
      break;
    case AUX_TO_MEX:
      this->auxiliaries << "#ifdef MATLAB_MEX_FILE\n"
                        << sanitize_source(casadi_to_mex_str, inst)
                        << "#endif\n\n";
      break;
    case AUX_FROM_MEX:
      add_auxiliary(AUX_FILL);
      this->auxiliaries << "#ifdef MATLAB_MEX_FILE\n"
                        << sanitize_source(casadi_from_mex_str, inst)
                        << "#endif\n\n";
      break;
    case AUX_FINITE_DIFF:
      this->auxiliaries << sanitize_source(casadi_finite_diff_str, inst);
      break;
    }
  }

  std::string CodeGenerator::to_mex(const Sparsity& sp, const std::string& arg) {
    add_auxiliary(AUX_TO_MEX);
    stringstream s;
    s << "casadi_to_mex(" << sparsity(sp) << ", " << arg << ");";
    return s.str();
  }

  std::string CodeGenerator::from_mex(std::string& arg,
                                      const std::string& res, std::size_t res_off,
                                      const Sparsity& sp_res, const std::string& w) {
    // Handle offset with recursion
    if (res_off!=0) return from_mex(arg, res+"+"+str(res_off), 0, sp_res, w);

    add_auxiliary(AUX_FROM_MEX);
    stringstream s;
    s << "casadi_from_mex(" << arg
      << ", " << res << ", " << sparsity(sp_res) << ", " << w << ");";
    return s.str();
  }

  std::string CodeGenerator::constant(double v) {
    stringstream s;
    if (isnan(v)) {
      s << "NAN";
    } else if (isinf(v)) {
      if (v<0) s << "-";
      s << "INFINITY";
    } else {
      int v_int(v);
      if (v_int==v) {
        // Print integer
        s << v_int << ".";
      } else {
        // Print real
        std::ios_base::fmtflags fmtfl = s.flags(); // get current format flags
        s << std::scientific << std::setprecision(std::numeric_limits<double>::digits10 + 1) << v;
        s.flags(fmtfl); // reset current format flags
      }
    }
    return s.str();
  }

  std::string CodeGenerator::initializer(const vector<double>& v) {
    stringstream s;
    s << "{";
    for (int i=0; i<v.size(); ++i) {
      if (i!=0) s << ", ";
      s << constant(v[i]);
    }
    s << "}";
    return s.str();
  }

  std::string CodeGenerator::initializer(const vector<int>& v) {
    stringstream s;
    s << "{";
    for (int i=0; i<v.size(); ++i) {
      if (i!=0) s << ", ";
      s << v[i];
    }
    s << "}";
    return s.str();
  }

  std::string CodeGenerator::copy(const std::string& arg,
                                  std::size_t n, const std::string& res) {
    stringstream s;
    // Perform operation
    add_auxiliary(AUX_COPY);
    s << "casadi_copy(" << arg << ", " << n << ", " << res << ");";
    return s.str();
  }

  std::string CodeGenerator::fill(const std::string& res,
                                  std::size_t n, const std::string& v) {
    stringstream s;
    // Perform operation
    add_auxiliary(AUX_FILL);
    s << "casadi_fill(" << res << ", " << n << ", " << v << ");";
    return s.str();
  }

  std::string CodeGenerator::dot(int n, const std::string& x,
                                 const std::string& y) {
    add_auxiliary(AUX_DOT);
    stringstream s;
    s << "casadi_dot(" << n << ", " << x << ", " << y << ")";
    return s.str();
  }

  std::string CodeGenerator::bilin(const std::string& A, const Sparsity& sp_A,
                                   const std::string& x, const std::string& y) {
    add_auxiliary(AUX_BILIN);
    stringstream s;
    s << "casadi_bilin(" << A << ", " << sparsity(sp_A) << ", " << x << ", " << y << ")";
    return s.str();
  }

  std::string CodeGenerator::rank1(const std::string& A, const Sparsity& sp_A,
                                   const std::string& alpha, const std::string& x,
                                   const std::string& y) {
    add_auxiliary(AUX_RANK1);
    stringstream s;
    s << "casadi_rank1(" << A << ", " << sparsity(sp_A) << ", "
      << alpha << ", " << x << ", " << y << ");";
    return s.str();
  }

  std::string CodeGenerator::interpn(int ndim, const std::string& grid, const std::string& offset,
                                   const std::string& values, const std::string& x,
                                   const std::string& lookup_mode,
                                   const std::string& iw, const std::string& w) {
    add_auxiliary(AUX_INTERPN);
    stringstream s;
    s << "casadi_interpn(" << ndim << ", " << grid << ", "  << offset << ", "
      << values << ", " << x << ", " << lookup_mode << ", " << iw << ", " << w << ");";
    return s.str();
  }

  std::string CodeGenerator::interpn_grad(const std::string& grad,
                                   int ndim, const std::string& grid, const std::string& offset,
                                   const std::string& values, const std::string& x,
                                   const std::string& lookup_mode,
                                   const std::string& iw, const std::string& w) {
    add_auxiliary(AUX_INTERPN_GRAD);
    stringstream s;
    s << "casadi_interpn_grad(" << grad << ", " << ndim << ", " << grid << ", " << offset << ", "
      << values << ", " << x << ", " << lookup_mode << ", " << iw << ", " << w << ");";
    return s.str();
  }

  std::string CodeGenerator::trans(const std::string& x, const Sparsity& sp_x,
                                   const std::string& y, const Sparsity& sp_y,
                                   const std::string& iw) {
    add_auxiliary(CodeGenerator::AUX_TRANS);
    return "casadi_trans(" + x + "," + sparsity(sp_x) + ", "
            + y + ", " + sparsity(sp_y) + ", " + iw + ")";
  }

  std::string CodeGenerator::declare(std::string s) {
    // Add c linkage
    string cpp_prefix = this->cpp ? "extern \"C\" " : "";

    // To header file
    if (this->with_header) {
      this->header << cpp_prefix << s << ";\n";
    }

    // Return name with declarations
    return cpp_prefix + this->dll_export + s;
  }

  std::string
  CodeGenerator::project(const std::string& arg, const Sparsity& sp_arg,
                         const std::string& res, const Sparsity& sp_res,
                         const std::string& w) {
    // If sparsity match, simple copy
    if (sp_arg==sp_res) return copy(arg, sp_arg.nnz(), res);

    // Create call
    add_auxiliary(AUX_PROJECT);
    stringstream s;
    s << "casadi_project(" << arg << ", " << sparsity(sp_arg) << ", " << res << ", "
      << sparsity(sp_res) << ", " << w << ");";
    return s.str();
  }

  std::string CodeGenerator::printf(const std::string& str, const std::vector<std::string>& arg) {
    add_include("stdio.h");
    stringstream s;
    s << "PRINTF(\"" << str << "\"";
    for (int i=0; i<arg.size(); ++i) s << ", " << arg[i];
    s << ");";
    return s.str();
  }

  std::string CodeGenerator::printf(const std::string& str, const std::string& arg1) {
    std::vector<std::string> arg;
    arg.push_back(arg1);
    return printf(str, arg);
  }

  std::string CodeGenerator::printf(const std::string& str, const std::string& arg1,
                                    const std::string& arg2) {
    std::vector<std::string> arg;
    arg.push_back(arg1);
    arg.push_back(arg2);
    return printf(str, arg);
  }

  std::string CodeGenerator::printf(const std::string& str, const std::string& arg1,
                                    const std::string& arg2, const std::string& arg3) {
    std::vector<std::string> arg;
    arg.push_back(arg1);
    arg.push_back(arg2);
    arg.push_back(arg3);
    return printf(str, arg);
  }

  std::string CodeGenerator::axpy(int n, const std::string& a,
                                  const std::string& x, const std::string& y) {
    add_auxiliary(AUX_AXPY);
    return "casadi_axpy(" + str(n) + ", " + a + ", " + x + ", " + y + ");";
  }

  std::string CodeGenerator::scal(int n, const std::string& alpha, const std::string& x) {
    add_auxiliary(AUX_SCAL);
    return "casadi_scal(" + str(n) + ", " + alpha + ", " + x + ");";
  }

  std::string CodeGenerator::mv(const std::string& x, const Sparsity& sp_x,
                                const std::string& y, const std::string& z, bool tr) {
    add_auxiliary(AUX_MV);
    return "casadi_mv(" + x + ", " + sparsity(sp_x) + ", " + y + ", "
           + z + ", " +  (tr ? "1" : "0") + ");";
  }

  std::string CodeGenerator::mv(const std::string& x, int nrow_x, int ncol_x,
                                const std::string& y, const std::string& z, bool tr) {
    add_auxiliary(AUX_MV_DENSE);
    return "casadi_mv_dense(" + x + ", " + str(nrow_x) + ", " + str(ncol_x) + ", "
           + y + ", " + z + ", " +  (tr ? "1" : "0") + ");";
  }

  std::string CodeGenerator::mtimes(const std::string& x, const Sparsity& sp_x,
                                    const std::string& y, const Sparsity& sp_y,
                                    const std::string& z, const Sparsity& sp_z,
                                    const std::string& w, bool tr) {
    add_auxiliary(AUX_MTIMES);
    return "casadi_mtimes(" + x + ", " + sparsity(sp_x) + ", " + y + ", " + sparsity(sp_y) + ", "
      + z + ", " + sparsity(sp_z) + ", " + w + ", " +  (tr ? "1" : "0") + ");";
  }

  void CodeGenerator::print_formatted(const std::string& s) {
    // Quick return if empty
    if (s.empty()) return;

    // If new line, add indentation
    if (newline_) {
      int shift = s.front()=='}' ? -1 : 0;
      casadi_assert_dev(current_indent_+shift>=0);
      this->buffer << string(indent_*(current_indent_+shift), ' ');
      newline_ = false;
    }

    // Print to body
    this->buffer << s;

    // Brackets change indentation for next row
    // NOTE(@jaeandersson): Should ignore strings, comments
    for (char c : s) {
      if (c=='{') {
        indent();
      } else if (c=='}') {
        unindent();
      }
    }
  }

  CodeGenerator& CodeGenerator::operator<<(const string& s) {
    // Loop over newline characters
    size_t off=0;
    while (true) {
      size_t pos = s.find('\n', off);
      if (pos==string::npos) {
        // No more newline characters
        print_formatted(s.substr(off));
        break;
      } else {
        // Ends with newline
        print_formatted(s.substr(off, pos-off));
        this->buffer << '\n';
        newline_ = true;
        off = pos+1;
      }
    }

    return *this;
  }

  void CodeGenerator::flush(std::ostream &s) {
    s << this->buffer.str();
    this->buffer.str(string());
  }

  void CodeGenerator::local(const std::string& name, const std::string& type,
                            const std::string& ref) {
    // Check if the variable already exists
    auto it = local_variables_.find(name);
    if (it==local_variables_.end()) {
      // Add it
      local_variables_[name] = make_pair(type, ref);
    } else {
      // Consistency check
      casadi_assert(it->second.first==type, "Type mismatch for " + name);
      casadi_assert(it->second.second==ref, "Type mismatch for " + name);
    }
  }

  void CodeGenerator::init_local(const string& name, const string& def) {
    bool inserted = local_default_.insert(make_pair(name, def)).second;
    casadi_assert(inserted, name + " already defined");
  }

  string CodeGenerator::
  sanitize_source(const std::string& src,
                  const vector<string>& inst, bool add_shorthand) {
    // Create suffix if templates type are not all "casadi_real"
    string suffix;
    for (const string& s : inst) {
      if (s!="casadi_real") {
        for (const string& s : inst) suffix += "_" + s;
        break;
      }
    }

    // Construct map of name replacements
    std::vector<std::pair<std::string, std::string> > rep;
    for (int i=0; i<inst.size(); ++i) {
      rep.push_back(make_pair("T" + str(i+1), inst[i]));
    }

    // Return object
    stringstream ret;
    // Process C++ source
    string line;
    istringstream stream(src);
    while (std::getline(stream, line)) {
      size_t n1, n2;

      // C++ template declarations are ignored
      if (line.find("template")==0) continue;

      // Macro definitions are ignored
      if (line.find("#define")==0) continue;
      if (line.find("#undef")==0) continue;

      // Inline declaration
      if (line == "inline") continue;

      // If line starts with "// SYMBOL", add shorthand
      // If line starts with "// C-REPLACE", add to list of replacements
      if (line.find("// SYMBOL")==0) {
        n1 = line.find("\"");
        n2 = line.find("\"", n1+1);
        string sym = line.substr(n1+1, n2-n1-1);
        if (add_shorthand) shorthand(sym + suffix);
        if (!suffix.empty()) {
          rep.push_back(make_pair(sym, sym + suffix));
        }
        continue;
      }

      // If line starts with "// C-REPLACE", add to list of replacements
      if (line.find("// C-REPLACE")==0) {
        // Get C++ string
        n1 = line.find("\"");
        n2 = line.find("\"", n1+1);
        string key = line.substr(n1+1, n2-n1-1);
        // Get C string
        n1 = line.find("\"", n2+1);
        n2 = line.find("\"", n1+1);
        string sub = line.substr(n1+1, n2-n1-1);
        // Add to replacements
        rep.push_back(make_pair(key, sub));
        continue;
      }

      // Ignore other C++ style comment
      if ((n1 = line.find("//")) != string::npos) line.erase(n1);

      // Remove trailing spaces
      if ((n1 = line.find_last_not_of(' ')) != string::npos) {
        line.erase(n1 + 1);
      } else {
        continue;
      }

      // Perform string replacements
      for (auto&& it = rep.rbegin(); it!=rep.rend(); ++it) {
        string::size_type n = 0;
        while ((n = line.find(it->first, n)) != string::npos) {
          line.replace(n, it->first.size(), it->second);
          n += it->second.size();
        }
      }

      // Append to return
      ret << line << "\n";
    }

    // Trailing newline
    ret << "\n";
    return ret.str();
  }

  void CodeGenerator::comment(const std::string& s) {
    if (verbose) {
      *this << "/* " << s << " */\n";
    }
  }

  void CodeGenerator::
  add_io_sparsities(const std::string& name,
                    const std::vector<Sparsity>& sp_in,
                    const std::vector<Sparsity>& sp_out) {
    // Insert element, quick return if it already exists
    if (!sparsity_meta.insert(name).second) return;

    // Input sparsities
    *this << declare("const int* " + name + "_sparsity_in(int i)") << " {\n"
      << "switch (i) {\n";
    for (int i=0; i<sp_in.size(); ++i) {
      *this << "case " << i << ": return " << sparsity(sp_in[i]) << ";\n";
    }
    *this << "default: return 0;\n}\n"
      << "}\n\n";

    // Output sparsities
    *this << declare("const int* " + name + "_sparsity_out(int i)") << " {\n"
      << "switch (i) {\n";
    for (int i=0; i<sp_out.size(); ++i) {
      *this << "case " << i << ": return " << sparsity(sp_out[i]) << ";\n";
    }
    *this << "default: return 0;\n}\n"
      << "}\n\n";
  }

} // namespace casadi
