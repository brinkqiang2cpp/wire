/*
 * cpp_generator.cpp
 *
 *  Created on: 27 апр. 2016 г.
 *      Author: sergey.fedorov
 */

#include <wire/wire2cpp/cpp_generator.hpp>

#include <iostream>
#include <cctype>
#include <algorithm>
#include <boost/filesystem.hpp>

#include <wire/idl/syntax_error.hpp>

namespace wire {
namespace idl {
namespace cpp {

namespace {

::std::string const autogenerated =
R"~(/*
 * THIS FILE IS AUTOGENERATED BY wire2cpp PROGRAM
 * Any manual modifications can be lost
 */
)~";

struct type_mapping {
    ::std::string                     type_name;
    ::std::vector<::std::string>      headers;
};

::std::map< ::std::string, type_mapping > const wire_to_cpp = {
    /* Wire type      C++ Type                  Headers                                      */
    /*--------------+-------------------------+----------------------------------------------*/
    { "void",       { "void",                   {}                                          } },
    { "bool",       { "bool",                   {}                                          } },
    { "char",       { "char",                   {}                                          } },
    { "byte",       { "::std::int8_t",          {"<cstdint>"}                               } },
    { "int32",      { "::std::int32_t",         {"<cstdint>"}                               } },
    { "int64",      { "::std::int64_t",         {"<cstdint>"}                               } },
    { "octet",      { "::std::uint8_t",         {"<cstdint>"}                               } },
    { "uint32",     { "::std::int32_t",         {"<cstdint>"}                               } },
    { "uint64",     { "::std::int64_t",         {"<cstdint>"}                               } },
    { "float",      { "float",                  {}                                          } },
    { "double",     { "double",                 {}                                          } },
    { "string",     { "::std::string",          {"<string>"}                                } },
    { "uuid",       { "::boost::uuids::uuid",   {"<boost/uuid/uuid.hpp>",
                                                "<wire/encoding/detail/uuid_io.hpp>"}       } },

    { "variant",    { "::boost::variant",       {"<boost/variant.hpp>",
                                                "<wire/encoding/detail/variant_io.hpp>"}    } },
    { "optional",   { "::boost::optional",      {"<boost/optional.hpp>",
                                                "<wire/encoding/detail/optional_io.hpp>"}   } },
    { "sequence",   { "::std::vector",          {"<vector>",
                                                "<wire/encoding/detail/containers_io.hpp>"} } },
    { "array",      { "::std::array",           {"<array>",
                                                "<wire/encoding/detail/containers_io.hpp>"} } },
    { "dictionary", { "::std::map",             {"<map>",
                                                "<wire/encoding/detail/containers_io.hpp>"} } },

};

::std::size_t tab_width = 4;

}  /* namespace  */

::std::ostream&
operator << (::std::ostream& os, offset const& val)
{
    ::std::ostream::sentry s(os);
    if (s) {
        for (size_t i = 0; i < val.sz * tab_width; ++i) {
            os << " ";
        }
    }
    return os;
}


namespace fs = ::boost::filesystem;

generator::generator(generate_options const& opts, preprocess_options const& ppo,
        ast::global_namespace_ptr ns)
    : ns_{ns}, unit_{ns->current_compilation_unit()},
      current_scope_{true}
{
    auto cwd = fs::current_path();
    ::std::cerr << "Current dir " << cwd << "\n";

    fs::path origin(unit_->name);
    fs::path header_path{ opts.header_output_dir };
    fs::path source_path{ opts.source_output_dir };

    ::std::string origin_path = origin.parent_path().string();

    if (header_path.empty()) {
        header_path = fs::current_path();
    }
    if (header_path.is_relative()) {
        header_path = fs::absolute(header_path);
    }
    if (!fs::exists(header_path)) {
        fs::create_directories(header_path);
    }
    header_path = fs::canonical(header_path);

    header_path /= origin.filename().stem().string() + ".hpp";

    if (source_path.empty()) {
        source_path = fs::current_path();
    }
    if (source_path.is_relative()) {
        source_path = fs::absolute(source_path);
    }
    if (!fs::exists(source_path)) {
        fs::create_directories(source_path);
    }
    source_path = fs::canonical(source_path);

    source_path /= origin.filename().stem().string() + ".cpp";

    string_list includes_rooted;

    for (auto const& inc : ppo.include_dirs) {
        fs::path in_path(inc);
        if (in_path.is_relative() && fs::exists(in_path)) {
            in_path = fs::canonical(in_path);
        }
        if (fs::exists(in_path)) {
            ::std::cerr << "Include directory " << in_path << "\n";
            includes_rooted.push_back(in_path.string());
        }
    }

    header_guard_ = "_" + opts.header_include_dir + "_" +
            origin.stem().string() + "_HPP_";
    ::std::transform(header_guard_.begin(), header_guard_.end(),
            header_guard_.begin(), toupper);

    header_.open(header_path.string());
    source_.open(source_path.string());
    header_ << autogenerated
            << "#ifndef " << header_guard_ << "\n"
            "#define " << header_guard_ << "\n\n";
    source_ << autogenerated;

    if (!opts.header_include_dir.empty()) {
        source_ << "#include <" << opts.header_include_dir << "/"
                << origin.stem().string() << ".hpp>\n";
    } else {
        source_ << "#include \"" << origin.stem().string() << ".hpp\"\n";
    }

    //------------------------------------------------------------------------
    ::std::cerr << "Will generate declarations for the following:\n"
        << origin.filename().string()
        << " -> " << header_path << " " << source_path << "\n";

    for (auto const& e : unit_->entities) {
        ::std::cerr << "\t" << e->get_qualified_name() << "\n";
    }

    ::std::set< ::std::string > standard_headers;
    ::std::cerr << "Dependencies of this compilation unit:\n";
    auto deps = unit_->external_dependencies();
    for (auto const& d : deps) {
        ::std::cerr << "\t" << d->get_qualified_name() << "\n";
        if (auto t = ast::dynamic_entity_cast< ast::type >(d)) {
            if (ast::type::is_built_in(d->get_qualified_name())) {
                auto const& tm = wire_to_cpp.at(d->name());
                if (!tm.headers.empty()) {
                    standard_headers.insert(tm.headers.begin(), tm.headers.end());
                }
            }
        }
    }

    // Collect custom headers and types from type aliases
//    ast::entity_const_set type_aliases;
//    for (auto const& e : unit_->entities) {
//        e->collect_elements(type_aliases,
//        [](ast::entity_const_ptr e){
//            return ast::dynamic_entity_cast< ast::type_alias >(e).get();
//        });
//    }
//
//    for (auto const& ta : type_aliases) {
//        ;
//    }

    for (auto const& hdr : standard_headers) {
        header_ << "#include " << hdr << "\n";
    }
    header_ << "#include <wire/encoding/wire_io.hpp>\n";
    //------------------------------------------------------------------------
    ::std::cerr << "Depends on compilation units:\n";
    auto units = unit_->dependent_units();
    for (auto const& u : units) {

        if (u->is_builtin()) {
            ::std::cerr << "\t" << u->name << "\n";
            //header_ << "#include <wire/types.hpp>\n";
        } else {
            fs::path fp(u->name);
            if (fp.is_relative() && !fp.has_parent_path()) {
                ::std::cerr << "\t" << u->name << "\n";
                if (!opts.header_include_dir.empty()) {
                    header_ << "#include <" << opts.header_include_dir << "/"
                            << fp.stem().string() << ".hpp>\n";
                } else {
                    header_ << "#include \"" << fp.stem().string() << ".hpp\"\n";
                }
            } else  if (u->name.find(origin_path) == 0) {
                // Same directory
                ::std::cerr << "\t" << u->name.substr(origin_path.size() + 1) << "\n";
                if (!opts.header_include_dir.empty()) {
                    header_ << "#include <" << opts.header_include_dir << "/"
                            << fp.stem().string() << ".hpp>\n";
                } else {
                    header_ << "#include \"" << fp.stem().string() << ".hpp\"\n";
                }
            } else {
                // Try to cut off the include root or current
                for (auto const& inc : includes_rooted) {
                    if (u->name.find(inc) == 0) {
                        fs::path ipath{u->name.substr(inc.size() + 1)};
                        ::std::cerr << "\t" << ipath << "\n";
                        header_ << "#include <" << ipath.parent_path().string() << "/"
                                << ipath.stem().string() << ".hpp>\n";
                        break;
                    }
                }
            }
        }
    }
    header_ << "\n";
    source_ << "\n";
}

generator::~generator()
{
    adjust_scope(qname{}.search());
    header_ << "\n#endif /* " << header_guard_ << " */\n";
}

void
generator::adjust_scope(qname_search const& target)
{
    qname_search current = current_scope_.search();

    auto c = current.begin;
    auto t = target.begin;
    for (; c != current.end && t != target.end && *c == *t; ++c, ++t);

    auto erase_start = c;
    ::std::deque<::std::string> to_close;
    for (; c != current.end; ++c) {
        to_close.push_front(*c);
    }
    for (auto const& ns : to_close) {
        header_ << "} /* namespace " << ns << " */\n";
        source_ << "} /* namespace " << ns << " */\n";
    }

    if (!to_close.empty()) {
        header_ << "\n";
        source_ << "\n";
    }


    current_scope_.components.erase(erase_start, current_scope_.components.end());

    bool space = t != target.end;
    for (; t != target.end; ++t) {
        header_ << "namespace " << *t << " {\n";
        source_ << "namespace " << *t << " {\n";
        current_scope_.components.push_back(*t);
    }
    if (space) {
        header_ << "\n";
        source_ << "\n";
    }
}

::std::ostream&
 generator::write_qualified_name(::std::ostream& os, qname const& qn)
{
    qname_search current = current_scope_.search();
    qname_search target = qn.search(false);

    auto c = current.begin;

    for (; c != current.end && !target.empty() && *c == *target.begin;
            ++c, ++target.begin);

    if (target.empty()) {
        os << qn;
    } else {
        os << target;
    }

    return os;
}


::std::ostream&
generator::write_type_name(::std::ostream& os, ast::type_ptr t)
{
    if (auto pt = ast::dynamic_entity_cast< ast::parametrized_type >(t)) {
        os << wire_to_cpp.at(pt->name()).type_name << " <";
        for (auto p = pt->params().begin(); p != pt->params().end(); ++p) {
            if (p != pt->params().begin())
                os << ", ";
            switch (p->which()) {
                case ast::template_param_type::type:
                    write_type_name(os, ::boost::get< ast::type_ptr >(*p) );
                    break;
                case ast::template_param_type::integral:
                    os << ::boost::get< ::std::string >(*p);
                    break;
                default:
                    throw grammar_error(t->decl_position(),
                            "Unknown template parameter kind");
            }
        }
        os << ">";
    } else {
        if (ast::type::is_built_in(t->get_qualified_name())) {
            os << wire_to_cpp.at(t->name()).type_name;
        } else {
            write_qualified_name(os, t->get_qualified_name());
        }
    }
    return os;
}

::std::ostream&
generator::write_init(::std::ostream& os, grammar::data_initializer const& init)
{
    switch (init.value.which()) {
        case 0: {
            os << ::boost::get< ::std::string >(init.value);
            break;
        }
        case 1: {
            grammar::data_initializer::initializer_list const& list =
                    ::boost::get<grammar::data_initializer::initializer_list>(init.value);
            if (list.size() < 3) {
                os << "{";
                for (auto p = list.begin(); p != list.end(); ++p) {
                    if (p != list.begin())
                        os << ", ";
                    write_init(os, *(*p));
                }
                os << "}";
            } else {
                os << "{\n" << ++h_off_;
                for (auto p = list.begin(); p != list.end(); ++p) {
                    if (p != list.begin())
                        os << ",\n" << h_off_;
                    write_init(os, *(*p));
                }
                os << "\n" << --h_off_ << "}";
            }
            break;
        }
    }
    return os;
}


void
generator::generate_constant(ast::constant_ptr c)
{
    auto qn = c->get_qualified_name();
    adjust_scope(qn.search().scope());

    header_ << h_off_;
    write_type_name(header_, c->get_type()) << " const " << c->name() << " = ";
    write_init(header_, c->get_init());
    header_ << ";\n";
}

void
generator::generate_enum(ast::enumeration_ptr enum_)
{
    auto qn = enum_->get_qualified_name();
    adjust_scope(qn.search().scope());

    header_ << h_off_++ << "enum ";
    if (enum_->constrained())
        header_ << "class ";
    write_type_name(header_, enum_) << "{\n";
    for (auto e : enum_->get_enumerators()) {
        header_ << h_off_ << e.first;
        if (e.second.is_initialized()) {
            header_ << " = " << *e.second;
        }
        header_ << ",\n";
    }
    header_ << --h_off_ << "};\n\n";
}

void
generator::generate_type_alias( ast::type_alias_ptr ta )
{
    auto qn = ta->get_qualified_name();
    adjust_scope(qn.search().scope());

    header_ << h_off_ << "using " << ta->name() << " = ";
    // TODO Custom mapping
    write_type_name(header_, ta->alias());
    header_ << ";\n";
}

void
generator::generate_struct( ast::structure_ptr struct_ )
{
    auto qn = struct_->get_qualified_name();
    adjust_scope(qn.search().scope());

    header_ << "\n" << h_off_++ << "struct " << struct_->name() << " {\n";
    current_scope_.components.push_back(struct_->name());
    scope_stack_.push_back(struct_);

    for (auto t : struct_->get_types()) {
        generate_type_decl(t);
    }

    for (auto c : struct_->get_constants()) {
        generate_constant(c);
    }

    auto const& dm = struct_->get_data_members();
    for (auto d : dm) {
        header_ << h_off_;
        write_type_name(header_, d->get_type()) << " " << d->name() << ";\n";
    }

    if (!dm.empty()) {
        header_ << "\n" << h_off_ << "void\n"
                << h_off_ << "swap(" << struct_->name() << "& rhs)\n"
                << h_off_ << "{\n";

        header_ << ++h_off_ << "using ::std::swap;\n";
        for (auto d : dm) {
            header_ << h_off_ << "swap(" << d->name() << ", rhs." << d->name() << ");\n";
        }
        header_ << --h_off_ << "}\n";
    }

    header_ << --h_off_ << "};\n\n";
    current_scope_.components.pop_back();
    scope_stack_.pop_back();

    if (!dm.empty()) {
        if (scope_stack_.empty()) {
            for (auto f : free_functions_) {
                f();
            }
            free_functions_.clear();
            generate_read_write(struct_);
        } else {
            free_functions_.push_back(
                ::std::bind(&generator::generate_read_write, this, struct_));
        }
    }
}

void
generator::generate_read_write( ast::structure_ptr struct_)
{
    auto const& dm = struct_->get_data_members();
    header_ << h_off_ << "template < typename OutputIterator >\n"
            << h_off_ << "void\n"
            << h_off_ << "wire_write(OutputIterator o, ";
    write_type_name(header_, struct_) << " const& v)\n"
            << h_off_ << "{\n";

    header_ << ++h_off_ << "::wire::encoding::write(o";
    for (auto d : dm) {
        header_ << ", v." << d->name();
    }
    header_ << ");\n";
    header_ << --h_off_ << "}\n\n";

    header_ << h_off_ << "template < typename InputIterator >\n"
            << h_off_ << "void\n"
            << h_off_ << "wire_read(InputIterator& begin, InputIterator end, ";
    write_type_name(header_, struct_) << "& v)\n"
            << h_off_ << "{\n";

    header_ << ++h_off_;
    write_type_name(header_, struct_) << " tmp;\n"
            << h_off_ << "::wire::encoding::read(begin, end";
    for (auto d : dm) {
        header_ << ", v." << d->name();
    }
    header_ << ");\n"
            << h_off_ << "v.swap(tmp);\n";
    header_ << --h_off_ << "}\n\n";
}

}  /* namespace cpp */
}  /* namespace idl */
}  /* namespace wire */
