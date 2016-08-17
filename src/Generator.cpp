#include "Generator.h"
#include "Outputs.h"

namespace Halide {
namespace Internal {

namespace {

// Return true iff the name is valid for Generators or Params.
// (NOTE: gcc didn't add proper std::regex support until v4.9;
// we don't yet require this, hence the hand-rolled replacement.)

bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// Note that this includes '_'
bool is_alnum(char c) { return is_alpha(c) || (c == '_') || (c >= '0' && c <= '9'); }

// Basically, a valid C identifier, except:
//
// -- initial _ is forbidden (rather than merely "reserved")
// -- two underscores in a row is also forbidden
bool is_valid_name(const std::string& n) {
    if (n.empty()) return false;
    if (!is_alpha(n[0])) return false;
    for (size_t i = 1; i < n.size(); ++i) {
        if (!is_alnum(n[i])) return false;
        if (n[i] == '_' && n[i-1] == '_') return false;
    }
    return true;
}

std::string compute_base_path(const std::string &output_dir,
                              const std::string &function_name,
                              const std::string &file_base_name) {
    std::vector<std::string> namespaces;
    std::string simple_name = extract_namespaces(function_name, namespaces);
    std::string base_path = output_dir + "/" + (file_base_name.empty() ? simple_name : file_base_name);
    return base_path;
}

std::string get_extension(const std::string& def, const GeneratorBase::EmitOptions &options) {
    auto it = options.extensions.find(def);
    if (it != options.extensions.end()) {
        return it->second;
    }
    return def;
}

Outputs compute_outputs(const Target &target,
                        const std::string &base_path,
                        const GeneratorBase::EmitOptions &options) {
    const bool is_windows_coff = target.os == Target::Windows &&
                                !target.has_feature(Target::MinGW);
    Outputs output_files;
    if (options.emit_o) {
        // If the target arch is pnacl, then the output "object" file is
        // actually a pnacl bitcode file.
        if (target.arch == Target::PNaCl) {
            output_files.object_name = base_path + get_extension(".bc", options);
        } else if (is_windows_coff) {
            // If it's windows, then we're emitting a COFF file
            output_files.object_name = base_path + get_extension(".obj", options);
        } else {
            // Otherwise it is an ELF or Mach-o
            output_files.object_name = base_path + get_extension(".o", options);
        }
    }
    if (options.emit_assembly) {
        output_files.assembly_name = base_path + get_extension(".s", options);
    }
    if (options.emit_bitcode) {
        // In this case, bitcode refers to the LLVM IR generated by Halide
        // and passed to LLVM, for both the pnacl and ordinary archs
        output_files.bitcode_name = base_path + get_extension(".bc", options);
    }
    if (options.emit_h) {
        output_files.c_header_name = base_path + get_extension(".h", options);
    }
    if (options.emit_cpp) {
        output_files.c_source_name = base_path + get_extension(".cpp", options);
    }
    if (options.emit_stmt) {
        output_files.stmt_name = base_path + get_extension(".stmt", options);
    }
    if (options.emit_stmt_html) {
        output_files.stmt_html_name = base_path + get_extension(".html", options);
    }
    if (options.emit_static_library) {
        if (is_windows_coff) {
            output_files.static_library_name = base_path + get_extension(".lib", options);
        } else {
            output_files.static_library_name = base_path + get_extension(".a", options);
        }
    }
    return output_files;
}

void compile_module_to_filter(const Module &m,
                              const std::string &base_path,
                              const GeneratorBase::EmitOptions &options) {
    Outputs output_files = compute_outputs(m.target(), base_path, options);
    m.compile(output_files);
}

Argument to_argument(const Internal::Parameter &param) {
    Expr def, min, max;
    if (!param.is_buffer()) {
        def = param.get_scalar_expr();
        min = param.get_min_value();
        max = param.get_max_value();
    }
    return Argument(param.name(),
        param.is_buffer() ? Argument::InputBuffer : Argument::InputScalar,
        param.type(), param.dimensions(), def, min, max);
}

}  // namespace

const std::map<std::string, Halide::Type> &get_halide_type_enum_map() {
    static const std::map<std::string, Halide::Type> halide_type_enum_map{
        {"bool", Halide::Bool()},
        {"int8", Halide::Int(8)},
        {"int16", Halide::Int(16)},
        {"int32", Halide::Int(32)},
        {"uint8", Halide::UInt(8)},
        {"uint16", Halide::UInt(16)},
        {"uint32", Halide::UInt(32)},
        {"float32", Halide::Float(32)},
        {"float64", Halide::Float(64)}
    };
    return halide_type_enum_map;
}

int generate_filter_main(int argc, char **argv, std::ostream &cerr) {
    const char kUsage[] = "gengen [-g GENERATOR_NAME] [-f FUNCTION_NAME] [-o OUTPUT_DIR] [-r RUNTIME_NAME] [-e EMIT_OPTIONS] [-x EXTENSION_OPTIONS] [-n FILE_BASE_NAME] "
                          "target=target-string[,target-string...] [generator_arg=value [...]]\n\n"
                          "  -e  A comma separated list of files to emit. Accepted values are "
                          "[assembly, bitcode, cpp, h, html, o, static_library, stmt]. If omitted, default value is [static_library, h].\n"
                          "  -x  A comma separated list of file extension pairs to substitute during file naming, "
                          "in the form [.old=.new[,.old2=.new2]]\n";

    std::map<std::string, std::string> flags_info = { { "-f", "" },
                                                      { "-g", "" },
                                                      { "-o", "" },
                                                      { "-e", "" },
                                                      { "-n", "" },
                                                      { "-x", "" },
                                                      { "-r", "" }};
    std::map<std::string, std::string> generator_args;

    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            std::vector<std::string> v = split_string(argv[i], "=");
            if (v.size() != 2 || v[0].empty() || v[1].empty()) {
                cerr << kUsage;
                return 1;
            }
            generator_args[v[0]] = v[1];
            continue;
        }
        auto it = flags_info.find(argv[i]);
        if (it != flags_info.end()) {
            if (i + 1 >= argc) {
                cerr << kUsage;
                return 1;
            }
            it->second = argv[i + 1];
            ++i;
            continue;
        }
        cerr << "Unknown flag: " << argv[i] << "\n";
        cerr << kUsage;
        return 1;
    }

    std::string runtime_name = flags_info["-r"];

    std::vector<std::string> generator_names = GeneratorRegistry::enumerate();
    if (generator_names.size() == 0 && runtime_name.empty()) {
        cerr << "No generators have been registered and not compiling a standalone runtime\n";
        cerr << kUsage;
        return 1;
    }

    std::string generator_name = flags_info["-g"];
    if (generator_name.empty() && runtime_name.empty()) {
        // If -g isn't specified, but there's only one generator registered, just use that one.
        if (generator_names.size() > 1) {
            cerr << "-g must be specified if multiple generators are registered:\n";
            for (auto name : generator_names) {
                cerr << "    " << name << "\n";
            }
            cerr << kUsage;
            return 1;
        }
        generator_name = generator_names[0];
    }
    std::string function_name = flags_info["-f"];
    if (function_name.empty()) {
        // If -f isn't specified, assume function name = generator name.
        function_name = generator_name;
    }
    std::string output_dir = flags_info["-o"];
    if (output_dir.empty()) {
        cerr << "-o must always be specified.\n";
        cerr << kUsage;
        return 1;
    }
    if (generator_args.find("target") == generator_args.end()) {
        cerr << "Target missing\n";
        cerr << kUsage;
        return 1;
    }
    // it's OK for file_base_name to be empty: filename will be based on function name
    std::string file_base_name = flags_info["-n"];

    GeneratorBase::EmitOptions emit_options;
    // Ensure all flags start as false.
    emit_options.emit_static_library = emit_options.emit_h = false;

    std::vector<std::string> emit_flags = split_string(flags_info["-e"], ",");
    if (emit_flags.empty() || (emit_flags.size() == 1 && emit_flags[0].empty())) {
        // If omitted or empty, assume .a and .h
        emit_options.emit_static_library = emit_options.emit_h = true;
    } else {
        // If anything specified, only emit what is enumerated
        for (const std::string &opt : emit_flags) {
            if (opt == "assembly") {
                emit_options.emit_assembly = true;
            } else if (opt == "bitcode") {
                emit_options.emit_bitcode = true;
            } else if (opt == "stmt") {
                emit_options.emit_stmt = true;
            } else if (opt == "html") {
                emit_options.emit_stmt_html = true;
            } else if (opt == "cpp") {
                emit_options.emit_cpp = true;
            } else if (opt == "o") {
                emit_options.emit_o = true;
            } else if (opt == "h") {
                emit_options.emit_h = true;
            } else if (opt == "static_library") {
                emit_options.emit_static_library = true;
            } else if (!opt.empty()) {
                cerr << "Unrecognized emit option: " << opt
                     << " not one of [assembly, bitcode, cpp, h, html, o, static_library, stmt], ignoring.\n";
            }
        }
    }

    auto extension_flags = split_string(flags_info["-x"], ",");
    for (const std::string &x : extension_flags) {
        if (x.empty()) {
            continue;
        }
        auto ext_pair = split_string(x, "=");
        if (ext_pair.size() != 2) {
            cerr << "Malformed -x option: " << x << "\n";
            cerr << kUsage;
            return 1;
        }
        emit_options.extensions[ext_pair[0]] = ext_pair[1];
    }

    const auto target_string = generator_args["target"];
    auto target_strings = split_string(target_string, ",");
    std::vector<Target> targets;
    for (const auto &s : target_strings) {
        targets.push_back(Target(s));
    }

    if (!runtime_name.empty()) {
        if (targets.size() != 1) {
            cerr << "Only one target allowed here";
            return 1;
        }
        std::string base_path = compute_base_path(output_dir, runtime_name, "");
        Outputs output_files = compute_outputs(targets[0], base_path, emit_options);
        compile_standalone_runtime(output_files, targets[0]);
    }

    if (!generator_name.empty()) {
        std::string base_path = compute_base_path(output_dir, function_name, file_base_name);
        Outputs output_files = compute_outputs(targets[0], base_path, emit_options);
        auto module_producer = [&generator_name, &generator_args, &cerr]
            (const std::string &name, const Target &target) -> Module {
                auto sub_generator_args = generator_args;
                sub_generator_args["target"] = target.to_string();
                // Must re-create each time since each instance will have a different Target
                auto gen = GeneratorRegistry::create(generator_name, sub_generator_args);
                if (gen == nullptr) {
                    cerr << "Unknown generator: " << generator_name << "\n";
                    exit(1);
                }
                return gen->build_module(name);
            };
        if (targets.size() > 1) {
            compile_multitarget(function_name, output_files, targets, module_producer);
        } else {
            // compile_multitarget() will fail if we request anything but library and/or header,
            // so defer directly to Module::compile if there is a single target.
            module_producer(function_name, targets[0]).compile(output_files);
        }
    }

    return 0;
}

GeneratorParamBase::GeneratorParamBase(const std::string &name) : name(name) {
    ObjectInstanceRegistry::register_instance(this, 0, ObjectInstanceRegistry::GeneratorParam,
                                              this, nullptr);
}

GeneratorParamBase::~GeneratorParamBase() { ObjectInstanceRegistry::unregister_instance(this); }

/* static */
GeneratorRegistry &GeneratorRegistry::get_registry() {
    static GeneratorRegistry *registry = new GeneratorRegistry;
    return *registry;
}

/* static */
void GeneratorRegistry::register_factory(const std::string &name,
                                         std::unique_ptr<GeneratorFactory> factory) {
    user_assert(is_valid_name(name)) << "Invalid Generator name: " << name;
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    internal_assert(registry.factories.find(name) == registry.factories.end())
        << "Duplicate Generator name: " << name;
    registry.factories[name] = std::move(factory);
}

/* static */
void GeneratorRegistry::unregister_factory(const std::string &name) {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    internal_assert(registry.factories.find(name) != registry.factories.end())
        << "Generator not found: " << name;
    registry.factories.erase(name);
}

/* static */
std::unique_ptr<GeneratorBase> GeneratorRegistry::create(const std::string &name,
                                                         const GeneratorParamValues &params) {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = registry.factories.find(name);
    user_assert(it != registry.factories.end()) << "Generator not found: " << name;
    return it->second->create(params);
}

/* static */
std::vector<std::string> GeneratorRegistry::enumerate() {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::vector<std::string> result;
    for (const auto& i : registry.factories) {
        result.push_back(i.first);
    }
    return result;
}

GeneratorBase::GeneratorBase(size_t size, const void *introspection_helper) : size(size), params_built(false) {
    ObjectInstanceRegistry::register_instance(this, size, ObjectInstanceRegistry::Generator, this, introspection_helper);
}

GeneratorBase::~GeneratorBase() { ObjectInstanceRegistry::unregister_instance(this); }

void GeneratorBase::rebuild_params() {
    params_built = false;
    filter_inputs.clear();
    filter_params.clear();
    generator_params.clear();
    build_params();
}

void GeneratorBase::build_params() {
    if (!params_built) {
        std::vector<void *> vf = ObjectInstanceRegistry::instances_in_range(
            this, size, ObjectInstanceRegistry::FilterParam);
        for (auto v : vf) {
            auto param = static_cast<Parameter *>(v);
            internal_assert(param != nullptr);
            user_assert(param->is_explicit_name()) << "Params in Generators must have explicit names: " << param->name();
            user_assert(is_valid_name(param->name())) << "Invalid Param name: " << param->name();
            for (auto p : filter_params) {
                user_assert(p->name() != param->name()) << "Duplicate Param name: " << param->name();
            }
            filter_params.push_back(param);
        }

        std::vector<void *> vi = ObjectInstanceRegistry::instances_in_range(
            this, size, ObjectInstanceRegistry::GeneratorInput);
        for (auto v : vi) {
            auto input = static_cast<Internal::GeneratorInputBase *>(v);
            internal_assert(input != nullptr);
            user_assert(is_valid_name(input->name())) << "Invalid Input name: (" << input->name() << ")\n";
            for (auto i : filter_inputs) {
                user_assert(i->name() != input->name()) << "Duplicate Input name: (" << input->name() << ")\n";
            }
            filter_inputs.push_back(input);
        }

        if (filter_params.size() > 0 && filter_inputs.size() > 0) {
            user_error << "Input<> may not be used with Param<> or ImageParam in Generators.\n";
        }

        std::vector<void *> vg = ObjectInstanceRegistry::instances_in_range(
            this, size, ObjectInstanceRegistry::GeneratorParam);
        for (auto v : vg) {
            auto param = static_cast<GeneratorParamBase *>(v);
            internal_assert(param != nullptr);
            user_assert(is_valid_name(param->name)) << "Invalid GeneratorParam name: " << param->name;
            user_assert(generator_params.find(param->name) == generator_params.end())
                << "Duplicate GeneratorParam name: " << param->name;
            generator_params[param->name] = param;
        }
        params_built = true;
    }
}

std::vector<Argument> GeneratorBase::get_filter_arguments() {
    build_params();
    init_inputs();  // TODO(srj): not sure if we need this, unlikely we do
    std::vector<Argument> arguments;
    for (auto param : filter_params) {
        arguments.push_back(to_argument(*param));
    }
    for (auto input : filter_inputs) {
        arguments.push_back(to_argument(input->parameter_));
    }
    return arguments;
}

GeneratorParamValues GeneratorBase::get_generator_param_values() {
    build_params();
    GeneratorParamValues results;
    for (auto key_value : generator_params) {
        GeneratorParamBase *param = key_value.second;
        results[param->name] = param->to_string();
    }
    return results;
}

void GeneratorBase::set_generator_param_values(const GeneratorParamValues &params) {
    build_params();
    for (auto key_value : params) {
        const std::string &key = key_value.first;
        const std::string &value = key_value.second;
        auto param = generator_params.find(key);
        user_assert(param != generator_params.end())
            << "Generator has no GeneratorParam named: " << key;
        param->second->from_string(value);
    }
}

void GeneratorBase::init_inputs() {
    for (auto input : filter_inputs) {
        input->init_internals();
    }
}

std::vector<Argument> GeneratorBase::get_filter_output_types() {
    std::vector<Argument> output_types;
    Pipeline pipeline = build_pipeline();
    std::vector<Func> pipeline_results = pipeline.outputs();
    for (Func func : pipeline_results) {
        for (Halide::Type t : func.output_types()) {
            std::string name = "result_" + std::to_string(output_types.size());
            output_types.push_back(Halide::Argument(name, Halide::Argument::OutputBuffer, t, func.dimensions()));
        }
    }
    return output_types;
}

Module GeneratorBase::build_module(const std::string &function_name,
                                   const LoweredFunc::LinkageType linkage_type) {
    build_params();
    Pipeline pipeline = build_pipeline();
    // Building the pipeline may mutate the Params/ImageParams (but not Inputs).
    if (filter_params.size() > 0) {
        rebuild_params();
    }
    return pipeline.compile_to_module(get_filter_arguments(), function_name, target, linkage_type);
}

void GeneratorBase::emit_filter(const std::string &output_dir,
                                const std::string &function_name,
                                const std::string &file_base_name,
                                const EmitOptions &options) {
    std::string base_path = compute_base_path(output_dir, function_name, file_base_name);
    compile_module_to_filter(build_module(function_name), base_path, options);
}

GeneratorInputBase::GeneratorInputBase(const std::string &n, Type t, Kind kind, int dimensions) 
    : parameter_(t, /*is_buffer*/ kind == Function, dimensions, n, /*is_explicit_name*/ true, /*register_instance*/ false) {
    ObjectInstanceRegistry::register_instance(this, 0, ObjectInstanceRegistry::GeneratorInput,
                                              this, nullptr);
}

GeneratorInputBase::~GeneratorInputBase() { 
    ObjectInstanceRegistry::unregister_instance(this); 
}

void GeneratorInputBase::init_internals() {
    if (parameter_.is_buffer()) {
        if (type_param && dimension_param) {
            parameter_ = Parameter(*type_param, /*is_buffer*/ true, *dimension_param, name(), true, false);
        } else if (type_param) {
            parameter_ = Parameter(*type_param, /*is_buffer*/ true, parameter_.dimensions(), name(), true, false);
        } else if (dimension_param) {
            parameter_ = Parameter(type(), /*is_buffer*/ true, *dimension_param, name(), true, false);
        }
        expr_ = Expr();
        func_ = Func(name() + "_im");
        std::vector<Var> args;
        std::vector<Expr> args_expr;
        for (int i = 0; i < parameter_.dimensions(); ++i) {
            args.push_back(Var::implicit(i));
            args_expr.push_back(Var::implicit(i));
        }
        func_(args) = Internal::Call::make(parameter_, args_expr);
    } else {
       expr_ = Internal::Variable::make(type(), name(), parameter_);
       func_ = Func();
    }
}

void generator_test() {
    GeneratorParam<int> gp("gp", 1);

    // Verify that RDom parameter-pack variants can convert GeneratorParam to Expr
    RDom rdom(0, gp, 0, gp);

    // Verify that Func parameter-pack variants can convert GeneratorParam to Expr
    Var x, y;
    Func f, g;
    f(x, y) = x + y;
    g(x, y) = f(gp, gp);                            // check Func::operator() overloads
    g(rdom.x, rdom.y) += f(rdom.x, rdom.y);
    g.update(0).reorder(rdom.y, rdom.x);            // check Func::reorder() overloads for RDom::operator RVar()

    // Verify that print() parameter-pack variants can convert GeneratorParam to Expr
    print(f(0, 0), g(1, 1), gp);
    print_when(true, f(0, 0), g(1, 1), gp);

    // Verify that Tuple parameter-pack variants can convert GeneratorParam to Expr
    Tuple t(gp, gp, gp);
}

}  // namespace Internal
}  // namespace Halide
