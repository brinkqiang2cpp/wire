/*
 * cpp_generator.cpp
 *
 *  Created on: 27 апр. 2016 г.
 *      Author: sergey.fedorov
 */

#include <wire/wire2cpp/cpp_generator.hpp>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <algorithm>
#include <boost/filesystem.hpp>

#include <wire/types.hpp>
#include <wire/idl/syntax_error.hpp>
#include <wire/util/murmur_hash.hpp>
#include <wire/wire2cpp/mapped_type.hpp>

namespace wire {
namespace idl {
namespace cpp {

namespace {

::std::map< ::std::string, date_and_time > const STRING_TO_DATE_AND_TIME {
    { "std",    date_and_time::std   },
    { "boost",  date_and_time::boost }
};

::std::map< date_and_time, ::std::string > const DATE_AND_TIME_TO_STRING {
    { date_and_time::std,   "std"   },
    { date_and_time::boost, "boost" }
};

} /* namespace  */

::std::ostream&
operator <<(::std::ostream& os, date_and_time val)
{
    ::std::ostream::sentry s(os);
    if (s) {
        auto f = DATE_AND_TIME_TO_STRING.find(val);
        if (f != DATE_AND_TIME_TO_STRING.end()) {
            os << f->second;
        } else {
            os << "Unknown team_size value " << (int)val;
        }
    }
    return os;
}

::std::istream&
operator >>(::std::istream& is, date_and_time& val)
{
    ::std::istream::sentry s(is);
    if (s) {
        ::std::string name;
        if (is >> name) {
            auto f = STRING_TO_DATE_AND_TIME.find(name);
            if (f != STRING_TO_DATE_AND_TIME.end()) {
                val = f->second;
            } else {
                is.setstate(::std::ios_base::failbit);
            }
        }
    }
    return is;
}

date_and_time generator::datetime_type{date_and_time::std};

namespace {

wire_type_map const builtin_types = {
    /* Wire type      C++ Type                  Headers                                      */
    /*--------------+-------------------------+----------------------------------------------*/
    { "void",       { "void",                   {}                                          } },
    { "bool",       { "bool",                   {}                                          } },
    { "char",       { "char",                   {}                                          } },
    { "byte",       { "::std::int8_t",          {"<cstdint>"}                               } },
    { "int16",      { "::std::int16_t",         {"<cstdint>"}                               } },
    { "int32",      { "::std::int32_t",         {"<cstdint>"}                               } },
    { "int64",      { "::std::int64_t",         {"<cstdint>"}                               } },
    { "octet",      { "::std::uint8_t",         {"<cstdint>"}                               } },
    { "uint16",     { "::std::uint16_t",        {"<cstdint>"}                               } },
    { "uint32",     { "::std::uint32_t",        {"<cstdint>"}                               } },
    { "uint64",     { "::std::uint64_t",        {"<cstdint>"}                               } },
    { "float",      { "float",                  {}                                          } },
    { "double",     { "double",                 {}                                          } },
    { "string",     { "::std::string",          {"<string>"}                                } },
    { "uuid",       { "::boost::uuids::uuid",   {"<boost/uuid/uuid.hpp>",
                                                "<boost/uuid/uuid_io.hpp>",
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


wire_type_map const chrono_mapping = {
        /* Wire type      C++ Type                  Headers                                      */
        /*--------------+-------------------------+----------------------------------------------*/
        { "time_point", { "::std::chrono::system_clock::time_point",
                                                    {"<chrono>",
                                                     "<wire/encoding/detail/chrono_io.hpp>"}    } },
        { "duration",   { "::std::chrono::system_clock::duration",
                                                    {"<chrono>",
                                                     "<wire/encoding/detail/chrono_io.hpp>"}    } }
};

wire_type_map const boost_time_mapping = {
        /* Wire type      C++ Type                  Headers                                      */
        /*--------------+-------------------------+----------------------------------------------*/
        { "time_point", { "::boost::posix_time::ptime",
                                             {"<wire/encoding/detail/boost_date_time_io.hpp>"} } },
        { "duration",   { "::boost::posix_time::time_duration",
                                             {"<wire/encoding/detail/boost_date_time_io.hpp>"} } }
};

} /* namespace  */

type_mapping const&
wire_to_cpp(::std::string const& name)
{
    if (chrono_mapping.find(name) != chrono_mapping.end()) {
        switch (generator::datetime_type) {
            case date_and_time::std :
                return chrono_mapping.at(name);
            case date_and_time::boost :
                return boost_time_mapping.at(name);
        }
    }
    return builtin_types.at(name);
}

namespace {

ast::qname const root_interface         { "::wire::core::object" };
ast::qname const root_exception         { "::wire::errors::user_exception" };
ast::qname const connection_failed      { "::wire::errors::connection_failed" };

ast::qname const wire_promise           { "::wire::promise" };

ast::qname const hash_value_type_name   { "::wire::hash_value_type" };
ast::qname const input_iterator_name    { "::wire::encoding::incoming::const_iterator" };
ast::qname const wire_outgoing          { "::wire::encoding::outgoing" };

ast::qname const back_inserter          { "::std::back_insert_iterator" };

ast::qname const wire_encoding_read     { "::wire::encoding::read" };
ast::qname const wire_encoding_write    { "::wire::encoding::write" };
ast::qname const wire_encoding_detail   { "::wire::encoding::detail" };
ast::qname const wire_util_namespace    { "::wire::util" };

ast::qname const wire_callback          { "::wire::core::functional::callback" };
ast::qname const wire_void_callback     { "::wire::core::functional::void_callback" };
ast::qname const wire_exception_callback{ "::wire::core::functional::exception_callback" };

ast::qname const wire_disp_request      { "::wire::core::detail::dispatch_request" };
ast::qname const wire_current           { "::wire::core::current" };
ast::qname const wire_no_current        { "::wire::core::no_current" };
ast::qname const wire_context           { "::wire::core::context_type" };
ast::qname const wire_no_context        { "::wire::core::no_context" };
ast::qname const invocation_opts        { "::wire::core::invocation_options" };
ast::qname const invocation_flags       { "::wire::core::invocation_flags" };
ast::qname const promise_inv_flags      { "::wire::core::promise_invocation_flags" };

ast::qname const wire_seg_head          { "::wire::encoding::segment_header" };
ast::qname const wire_seg_head_no_flags { "::wire::encoding::segment_header::none" };
ast::qname const wire_seg_head_last     { "::wire::encoding::segment_header::last_segment" };

ast::qname const wire_exception_init    { "::wire::errors::user_exception_factory_init" };
}  /* namespace  */

struct tmp_pop_scope {
    ast::qname&          scope;
    ::std::string   name;

    tmp_pop_scope(ast::qname& s)
        : scope{s}, name{s.components.back()}
    {
        scope.components.pop_back();
    }
    ~tmp_pop_scope()
    {
        scope.components.push_back(name);
    }
};

struct tmp_push_scope {
    ast::qname&          scope;
    ::std::string   name;

    tmp_push_scope(ast::qname& s, ::std::string const& n)
        : scope{s}, name{n}
    {
        scope.components.push_back(name);
    }
    ~tmp_push_scope()
    {
        if (!scope.components.empty() && scope.components.back() == name) {
            scope.components.pop_back();
        }
    }
};

grammar::annotation_list const mapped_type::empty_annotations{};

namespace fs = ::boost::filesystem;

generator::generator(generate_options const& opts, preprocess_options const& ppo,
        ast::global_namespace_ptr ns)
    : options_{opts}, ns_{ns}, unit_{ns->current_compilation_unit()},
      header_{ fs::path{unit_->name}.parent_path(),
            fs::path{ opts.header_include_dir }, ppo.include_dirs },
      source_{ fs::path{unit_->name}.parent_path(),
                fs::path{ opts.header_include_dir }, ppo.include_dirs },
      plugins_{ opts.plugins }
{
    auto cwd = fs::current_path();

    fs::path origin(unit_->name);
    fs::path header_path{ opts.header_output_dir };
    fs::path source_path{ opts.source_output_dir };

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

    header_.open(header_path);
    source_.open(source_path);
    header_ << "/* Hash for compilation unit is 0x"
            << ::std::hex << unit_->get_hash() << ::std::dec << " */\n";

    //------------------------------------------------------------------------
    ::std::set< ::std::string > standard_headers {"<memory>"};
    auto deps = unit_->external_dependencies();
    for (auto const& d : deps) {
        if (auto t = ast::dynamic_entity_cast< ast::type >(d)) {
            if (ast::type::is_built_in(qname(d))) {
                auto const& tm = wire_to_cpp(d->name());
                if (!tm.headers.empty()) {
                    standard_headers.insert(tm.headers.begin(), tm.headers.end());
                }
            }
        }
    }

    // Collect custom headers and types from type aliases
    ast::entity_const_set type_aliases;
    for (auto const& e : unit_->entities) {
        e->collect_elements(type_aliases,
        [](ast::entity_const_ptr e){
            return ast::dynamic_entity_cast< ast::type_alias >(e).get();
        });
    }

    for (auto ta : type_aliases) {
        auto const& anns = ta->get_annotations();
        auto f = find(anns, annotations::CPP_CONTAINER);
        if (f != anns.end()) {
            if (f->arguments.size() < 2)
                throw grammar_error(ta->decl_position(),
                        "Invalid cpp_container annotation");
            ::std::string header = f->arguments[1]->name;
            strip_quotes(header);
            standard_headers.insert(header);
        }
    }
    standard_headers.insert("<wire/encoding/wire_io.hpp>");
    header_.include(standard_headers);
    //------------------------------------------------------------------------
    auto units = unit_->dependent_units();
    for (auto const& u : units) {
        if (!u->is_builtin()) {
            header_.include(fs::path{ u->name });
        }
    }

    if (unit_->has_exceptions()) {
        header_.include("<wire/errors/user_exception.hpp>");
    }
    if (unit_->has_interfaces()) {
        header_.include({
            "<wire/core/object.hpp>",
            "<wire/core/functional.hpp>",
            "<wire/core/proxy.hpp>"
        });

        source_.include({"<wire/core/reference.hpp>",
            "<wire/core/connection.hpp>",
            "<wire/core/detail/dispatch_request.hpp>",
            "<wire/core/invocation.hpp>",
            "<unordered_map>",
            "<iomanip>",
            "<sstream>"
        });
    }
    if (unit_->has_classes()) {
        header_.include("<wire/encoding/detail/polymorphic_io.hpp>");
    }
    if (unit_->has_enums()) {
        header_.include("<wire/util/enum_range.hpp>");
    }

    header_ << "\n";
    source_ << "\n";

    plugins_.start_compilation_unit(*unit_, header_, source_);

    header_ << "\n";
    source_ << "\n";

    if (options_.dont_use_hashed_id) {
        ::std::cerr << "Don't use hashed type ids for marshalling\n";
    }

}

generator::~generator()
{
}

void generator::adjust_namespace(ast::entity_ptr ent)
{
    header_.adjust_namespace(ent);
    source_.adjust_namespace(ent);
}

::std::string
generator::constant_prefix(ast::qname const& qn) const
{
    ::std::ostringstream os;
    for (auto c : qn.components) {
        ::std::transform(c.begin(), c.end(), c.begin(), toupper);
        os << c << "_";
    }
    return os.str();
}

source_stream&
generator::write_data_member(source_stream& os, ast::variable_ptr var)
{
    os << off << mapped_type{var->get_type(), var->get_annotations()}
        << " " << cpp_name(var) << ";";
    return os;
}

void
generator::generate_dispatch_function_member(ast::function_ptr func)
{
    offset_guard hdr{header_};
    offset_guard src{source_};

    auto ann = find(func->get_annotations(), ast::annotations::SYNC);
    bool async_dispatch = ann == func->get_annotations().end();
    auto const& params = func->get_params();

    code_snippet formal_params { header_.current_scope() };
    code_snippet example_params { ast::qname{} };
    for (auto const& p : params) {
        formal_params << movable_arg_type(p.first) << " " << cpp_name(p.second) << ", ";
        example_params << movable_arg_type(p.first) << " " << cpp_name(p.second) << ", ";
    }

    if (async_dispatch) {
        header_ << off << "/* Async dispatch */";
        code_snippet ret_cb_name{header_.current_scope()};
        code_snippet ret_example{ast::qname{}};
        if (!func->is_void()) {
            ret_cb_name << cpp_name(func) << "_return_callback";
            ret_example << cpp_name(func) << "_return_callback";
            header_ << off << "using " << ret_cb_name << " = "
                << "::std::function< void("
                <<  arg_type(func->get_return_type(), func->get_annotations())
                << ") >;";
        } else {
            ret_cb_name << wire_void_callback;
            ret_example << wire_void_callback;
        }

        header_ << off << "virtual void"
                << off << cpp_name(func) << "(" << formal_params;
        if (!params.empty())
            header_ << off(+2);
        header_ << ret_cb_name << " __resp, "
                << off(+2) <<       wire_exception_callback << " __exception,"
                << off(+2) <<       wire_current << " const& = " << wire_no_current << ")";
        if (func->is_const()) {
            header_ << " const";
        }
        header_ << " = 0;";
        header_ << off << "/*  ----8<    copy here   8<----"
                << off << "void"
                << off << cpp_name(func) << "(" << example_params;
        if (!params.empty())
            header_ << off(+2);
        header_ << ret_example << " __resp, "
                << off(+2) << abs_name << wire_exception_callback << " __exception,"
                << off(+2) << abs_name << wire_current << " const& = "
                        << abs_name << wire_no_current << ")";
        if (func->is_const()) {
            header_ << " const";
        }
        header_ << " override;"
                << off << "    ---->8    end copy    >8----*/"
        ;
    } else {
        header_ << off << "/* Sync dispatch */"
                << off << "virtual "
                << mapped_type{func->get_return_type(), func->get_annotations()}
                << off << cpp_name(func) << "(" << formal_params
                << wire_current << " const& = " << wire_no_current << ")";
        if (func->is_const()) {
            header_ << " const";
        }
        header_ << " = 0;";
        header_ << off << "/*  ----8<    copy here   8<----"
                << off << abs_name << mapped_type{func->get_return_type(), func->get_annotations()}
                << off << cpp_name(func) << "(" << example_params
                    << abs_name << wire_current << " const& = "
                    << abs_name << wire_no_current << ")";
        if (func->is_const()) {
            header_ << " const";
        }
        header_ << " override;"
                << off << "    ---->8    end copy    >8----*/"
        ;
    }
    header_ << off << "/* dispatch function */"
            << off << "void"
            << off << "__" << cpp_name(func)
            << "(" << wire_disp_request << " const&, " << wire_current << " const&);\n";

    {
        source_ << off << "void"
                << off << qname(func->owner()) << "::__" << cpp_name(func)
                << "(" << wire_disp_request << " const& __req, " << wire_current << " const& __curr)"
                << off << "{";

        source_.modify_offset(+1);
        // Unmarshal params
        code_snippet unmarshal_params{ source_.current_scope() };
        code_snippet call_params{ source_.current_scope() };
        if (!params.empty()) {
            source_ << off << "auto __beg = __req.encaps_start;"
                    << off << "auto __end = __req.encaps_end;";
            for (auto p = params.begin(); p != params.end(); ++p) {
                source_ << off << mapped_type{p->first} << " " << p->second << ";";
                if (p != params.begin()) {
                    unmarshal_params << ", ";
                    call_params << ", ";
                }
                unmarshal_params << p->second;
                call_params << invoke_param( *p );
            }
            source_ << off << wire_encoding_read << "(__beg, __end, " << unmarshal_params << ");"
                    << off << "__req.encaps_start.incoming_encapsulation().read_indirection_table(__beg);";
        }

        code_snippet fcall{source_.current_scope()};
        fcall << cpp_name(func) << "(" << call_params;
        if (!params.empty())
            fcall << ", ";
        if (async_dispatch) {
            // Generate callback
            source_ << off << fcall;
            source_ << mod(+1) << "[__req](";
            if (!func->is_void()) {
                source_ <<  arg_type(func->get_return_type(), func->get_annotations())
                        << " _res";
            }
            source_ << ")"
                    << off << "{"
                    << mod(+1) << wire_outgoing << " __out{ __req.buffer->get_connector() };";
            if (!func->is_void()) {
                source_ << off
                        << wire_encoding_write << "(::std::back_inserter(__out), _res);";
            }
            source_ << off << "__req.result(::std::move(__out));";
            source_ << mod(-1) << "}, __req.exception, __curr);";
        } else {
            fcall << "__curr)";
            source_ << off << wire_outgoing << " __out{ __req.buffer->get_connector() };";
            if (func->is_void()) {
                source_ << off << fcall << ";";
            } else {
                source_ << off << wire_encoding_write << "(::std::back_inserter(__out), "
                        << fcall << ");";
            }
            source_ << off << "__req.result(::std::move(__out));";
        }

        source_ << mod(-2) << "}\n";
    }
}

void
generator::generate_invocation_function_member(ast::function_ptr func)
{
    offset_guard hdr{header_};
    offset_guard src{source_};

    auto pfx = constant_prefix(qname(func->owner()));

    code_snippet call_params{ header_.current_scope() };
    call_params.modify_offset(+2);
    auto const& params = func->get_params();
    for (auto const& p : params) {
        call_params << arg_type(p.first) << " " << p.second << "," << off;
    }

    // Sync invocation
    header_ << off << "/**"
            << off << " * Synchronous invocation of " << cpp_name(func)
            << off << " * Depending on the promise class will either throttle the I/O service (for ::std::promise)"
            << off << " * or just start an async operation (e.g. ::boost::fiber::promise)."
            << off << " */"
            << off << "template < template< typename > class _Promise = " << wire_promise << " >"
            << off << mapped_type{func->get_return_type(), func->get_annotations()}
            << off << cpp_name(func) << "(" << call_params << " "
            << wire_context << " const& __ctx = " << wire_no_context << ")"
            << off << "{"
            << mod(+1) << "auto future = " << cpp_name(func) << "_async<_Promise>(";
    for (auto const& p : params) {
        header_ << p.second << ", ";
    }
    header_ << "__ctx, "
            << off(+2)    << "wire_invocation_options() | "
                << promise_inv_flags << "<_Promise<" << mapped_type{func->get_return_type(), func->get_annotations()} << ">>::value);"
            << off << "return future.get();"
            << mod(-1) << "}\n";

    code_snippet result_callback{ header_.current_scope() };
    if (func->is_void()) {
        result_callback << wire_void_callback;
    } else {
        result_callback << cpp_name(func) << "_responce_callback";
        header_ << off << "using " << result_callback << " = "
                << "::std::function< void( "
                << movable_arg_type(func->get_return_type(), func->get_annotations())
                << " ) >;";
    }

    header_ << off << "/**"
            << off << " * Asynchronous invocation of " << cpp_name(func) << " actual implementation"
            << off << " * Starts async operation and passes completion callbacks"
            << off << " */"
            << off      << "void"
            << off      << cpp_name(func) << "_async(" << call_params << " "
            <<                      result_callback << " _response,"
            << mod(+2)  <<          wire_exception_callback << " _exception = nullptr,"
            << off      <<          wire_callback << "< bool > _sent        = nullptr,"
            << off      <<          wire_context    << " const&                       = " << wire_no_context << ","
            << off      <<          invocation_opts << " _opts                  = " << invocation_opts << "::unspecified);";
    header_.modify_offset(-2);

    header_ << off << "/**"
            << off << " * Async/sync transition helper function"
            << off << " */"
            << off      << "template < template< typename > class _Promise = " << wire_promise << " >"
            << off      << "auto"
            << off      << cpp_name(func) << "_async(" << call_params << " "
            << off      <<          wire_context << " const& _ctx                  = " << wire_no_context << ","
            << off(+2)  <<          invocation_opts << " const _opts = " << invocation_opts << "::unspecified)"
            << off(+2)  <<      "-> decltype( ::std::declval< _Promise< "
            <<                      mapped_type{func->get_return_type(), func->get_annotations()}
            <<                      " > >().get_future() )"
            << off      << "{"
            << mod(+1)  <<      "auto promise = ::std::make_shared< _Promise <"
                    << mapped_type{func->get_return_type(), func->get_annotations()} << "> >();\n";

    if (func->is_void()) {
        // Allow one-way invocation
        header_ << off      << "if (_opts.is_one_way()) {"
                << mod(+1)  <<    cpp_name(func) << "_async(";
        header_.modify_offset(+1);
        for (auto const& p : params) {
            header_ << off <<           p.second << ",";
        }
        header_ << off      <<           "nullptr,"
                << off      <<           "[promise]( ::std::exception_ptr ex )"
                << off      <<           "{ promise->set_exception(::std::move(ex)); },"
                << off      <<           "[promise](bool sent)"
                << off      <<            "{"
                << mod(+1)  <<                "if(sent) {"
                << off(+1)  <<                    "promise->set_value();"
                << off      <<                "} else {"
                << off(+1)  <<                    "promise->set_exception("
                << off(+2)  <<                         "::std::make_exception_ptr(" << connection_failed << "{}));"
                << off      <<                "}"
                << mod(-1)  <<            "}, _ctx, _opts);";

        header_ << mod(-2)  << "} else {";
        header_.modify_offset(+1);
    }
    header_ << off      <<      cpp_name(func) << "_async(";

    header_.modify_offset(+1);
    for (auto const& p : params) {
        header_ << off <<           p.second << ",";
    }
    if (func->is_void()) {
        header_ << off <<           "[promise]()"
                << off <<           "{ promise->set_value(); },";
    } else {
        header_ << off <<           "[promise](" << movable_arg_type(func->get_return_type(), func->get_annotations()) << " res)"
                << off <<           "{ promise->set_value(" << return_value(func->get_return_type(), "res") << "); },";
    }
    header_ << off     <<           "[promise]( ::std::exception_ptr ex )"
            << off     <<           "{ promise->set_exception(::std::move(ex)); },"
            << off     <<           "nullptr, _ctx, _opts"
            << mod(-1) <<       ");\n";

    if (func->is_void()) {
        header_ << mod(-1) << "}\n";
    }

    header_ << off     <<       "return promise->get_future();"
            << mod(-1) << "}\n";

    {
        // Sources
        {
            // Async invocation
            offset_guard _f{source_};
            source_ << off << "void"
                    << off << qname(func->owner()) << "_proxy::" << cpp_name(func) << "_async("
                    << mod(+3) << call_params << " "
                    << result_callback << " _response,"
                    << off << wire_exception_callback << " _exception,"
                    << off << wire_callback << "< bool > _sent,"
                    << off << wire_context << " const& _ctx,"
                    << off << invocation_opts << " _opts)"
                    << mod(-3) << "{";

            ::std::ostringstream os;
            os << pfx << cpp_name(func);
            if (!options_.dont_use_hashed_id && cpp_name(func).name.size() > 4) {
                // Name longer than hash
                os << "_hash";
            }

            source_ << mod(+1) << "if (_opts == " << invocation_opts << "::unspecified)"
                    << off(+1) << "_opts = wire_invocation_options();";
            source_ << off     << "make_invocation(wire_get_reference(),"
                    << mod(+2) << os.str() << ", _ctx,"
                    << off << "&" << qname(func->owner()) << "::" << cpp_name(func) << ","
                    << off << "_response, _exception, _sent";
            if (!params.empty()) {
                source_ << off;
                for (auto const& p : params) {
                    source_ << ", " << p.second;
                }
            }
            source_ << ")(_opts);";

            source_ << mod(-3) << "}\n";
        }
    }
}

void
generator::generate_forward_decl( ast::forward_declaration_ptr fwd )
{
    adjust_namespace(fwd);
    switch (fwd->kind()) {
        case ast::forward_declaration::structure:
            header_ << off << "struct " << qname(fwd) << ";";
            break;
        case ast::forward_declaration::interface:
            header_ << off << "class " << cpp_name(fwd) << "_proxy;"
                    << off << "using " << cpp_name(fwd) << "_prx = ::std::shared_ptr< "
                        << cpp_name(fwd) << "_proxy >;";
            ;
            // fallthrough
        case ast::forward_declaration::exception:
        case ast::forward_declaration::class_:
            header_ << off << "class " << cpp_name(fwd) << ";"
                    << off << "using " << cpp_name(fwd) << "_ptr = ::std::shared_ptr< "
                        << cpp_name(fwd) << ">;"
                    << off << "using " << cpp_name(fwd) << "_weak_ptr = ::std::weak_ptr< "
                        << cpp_name(fwd) << ">;"
            ;
            break;
        default:
            break;
    }
}

void
generator::generate_constant(ast::constant_ptr c)
{
    adjust_namespace(c);

    header_ << off << mapped_type{c->get_type(), c->get_annotations()}
            << " const " << cpp_name(c) << " = " << c->get_init() << ";";
}

void
generator::generate_enum(ast::enumeration_ptr enum_)
{
    adjust_namespace(enum_);
    offset_guard hdr{header_};

    header_ << off << "enum ";
    if (enum_->constrained())
        header_ << "class ";
    header_ << cpp_name(enum_) << "{";
    header_.modify_offset(+1);
    for (auto e : enum_->get_enumerators()) {
        header_ << off << cpp_name(e.first);
        if (e.second.is_initialized()) {
            header_ << " = " << *e.second;
        }
        header_ << ",";
    }
    header_ << mod(-1) << "};\n";

    plugins_.generate_enum(enum_, header_, source_);

    auto ann = find(enum_->get_annotations(), annotations::GENERATE_IO);
    if (ann != enum_->get_annotations().end()) {
        header_.at_namespace_scope(

        [enum_](source_stream& ss)
        {
            ss  << off << "::std::ostream&"
                << off << "operator << (::std::ostream&, " << qname(enum_) << " );"
                << off << "::std::istream&"
                << off << "operator >> ( ::std::istream&, " << qname(enum_) << "& );"
                << off << "::std::string"
                << off << "to_string( " << qname(enum_) << " );\n";
        });

        auto pfx = constant_prefix(qname(enum_));
        source_ << off << "// data for " << qname(enum_) << " enumeration IO"
                << off << "namespace {";

        source_ << mod(+1) << "::std::map < " << qname(enum_) << ", ::std::string > const "
                    << pfx << "TO_STRING {";
        source_.modify_offset(+1);

        for (auto e : enum_->get_enumerators()) {
            source_ << off << "{ " << qname(enum_, e.first) << ", \"" << e.first << "\" },";
        }
        source_ << mod(-1) << "};";

        source_ << off << "::std::map < ::std::string, " << qname(enum_) << " > const "
                    << pfx << "FROM_STRING {";
        source_.modify_offset(+1);
        for (auto e : enum_->get_enumerators()) {
            source_ << off << "{ \"" << e.first << "\", " << qname(enum_, e.first) << " },";
        }
        source_ << mod(-1) << "};";

        source_ << mod(-1) << "} // namespace\n";

        source_ << off  << "::std::ostream&"
                << off  << "operator << ( ::std::ostream& os, "
                           << qname(enum_) << " val )"
                << off      << "{"
                << mod(+1)  <<  "::std::ostream::sentry s{os};"
                << off      <<  "if (s) {"
                << mod(+1)  <<      "auto f = " << pfx << "TO_STRING.find(val);"
                << off      <<      "if (f != "<< pfx << "TO_STRING.end()) {"
                << off(+1)  <<          "os << f->second;"
                << off      <<      "} else {"
                << off(+1)  <<          "os << \"Unknown " << enum_->name() << " value \" << (int)val;"
                << off      <<      "}"
                << mod(-1)  <<  "}"
                << off      << "return os;"
                << mod(-1)  << "}\n";

        source_ << off  << "::std::istream&"
                << off  << "operator >> ( ::std::istream& is, "
                                << qname(enum_) << "& val )"
                << off  << "{"
                << mod(+1)  <<  "::std::istream::sentry s{is};"
                << off      <<  "if (s) {"
                << mod(+1)  <<      "::std::string name;"
                << off      <<      "if (is >> name) {"
                << mod(+1)  <<          "auto f = " << pfx << "FROM_STRING.find(name);"
                << off      <<          "if (f != " << pfx << "FROM_STRING.end()) {"
                << off(+1)  <<              "val = f->second;"
                << off      <<          "} else {"
                << off(+1)  <<              "is.setstate(::std::ios_base::failbit);"
                << off      <<          "}"
                << mod(-1)  <<      "}"
                << mod(-1)  <<  "}"
                << off      <<  "return is;"
                << mod(-1)  << "}\n";

        source_ << off      << "::std::string"
                << off      << "to_string( " << qname(enum_) << " val )"
                << off      << "{"
                << mod(+1)  <<      "auto f = " << pfx << "TO_STRING.find(val);"
                << off      <<      "if (f == "<< pfx << "TO_STRING.end()) {"
                << off(+1)  <<          "::std::ostringstream os;"
                << off(+1)  <<          "os << \"Unknown " << enum_->name() << " value \" << (int)val;"
                << off(+1)  <<          "throw ::std::runtime_error(os.str());"
                << off      <<      "}"
                << off      <<      "return f->second;"
                << mod(-1)  << "}\n";
    }
}

void
generator::generate_enum_traits(ast::enumeration_const_ptr enum_)
{
    if (!enum_->get_enumerators().empty()) {
        header_ << off      << "// enum_range traits for " << qname(enum_)
                << off      << "template <>"
                << off      << "struct enum_traits<" << qname(enum_) << "> {"
                << off(+1)      << "using enumerators = detail::enumerators<"
                << off(+4)              << qname(enum_) << ",";
        ;
        header_.modify_offset(+4);
        auto const& enumerators = enum_->get_enumerators();
        for (auto p = enumerators.begin(); p != enumerators.end(); ++p) {
            header_ << off << qname(enum_, p->first);
            auto n = p;
            if (++n != enumerators.end())
                header_ << ",";
        }
        header_.modify_offset(-4);
        header_ << off(+1)  << ">;";

        header_ << off << "};";
    }
}

void
generator::generate_type_alias( ast::type_alias_ptr ta )
{
    adjust_namespace(ta);

    header_ << off << "using " << cpp_name(ta) << " = "
            <<  mapped_type{ta->alias(), ta->get_annotations()} << ";";
}

void
generator::generate_struct( ast::structure_ptr struct_ )
{
    adjust_namespace(struct_);

    header_ << off << "struct " << cpp_name(struct_) << " {";
    auto const& dm = struct_->get_data_members();
    {
        header_.push_scope(struct_->name());

        for (auto t : struct_->get_types()) {
            generate_type_decl(t);
        }

        for (auto c : struct_->get_constants()) {
            generate_constant(c);
        }

        for (auto d : dm) {
            write_data_member(header_, d);
        }

        if (!dm.empty()) {
            header_ << "\n"
                    << off      << "void"
                    << off      << "swap(" << cpp_name(struct_) << "& rhs)"
                    << off      << "{"
                    << mod(+1)  <<      "using ::std::swap;";
            for (auto d : dm) {
                header_ << off  <<      "swap(" << cpp_name(d) << ", rhs." << cpp_name(d) << ");";
            }
            header_ << mod(-1) << "}";
        }

        plugins_.generate_struct(struct_, header_, source_);

        header_ << off << "};\n";
        header_.pop_scope();
    }

    if (!dm.empty()) {
        header_.at_namespace_scope(
        [this, struct_](source_stream& stream)
        {
            generate_read_write(stream, struct_);
        });
        header_.at_namespace_scope(
        [this, struct_](source_stream& stream)
        {
            generate_comparison(stream, struct_);
        });
        generate_io(struct_);
    }
}

void
generator::generate_read_write(source_stream& stream, ast::structure_ptr struct_)
{
    auto const& dm = struct_->get_data_members();
    stream  << off      << "template < typename OutputIterator >"
            << off      << "void"
            << off      << "wire_write(OutputIterator o, "
                                <<  qname(struct_) << " const& v)"
            << off      << "{";

    stream << off(+1)   <<      wire_encoding_write << "(o";
    for (auto d : dm) {
        stream << ", v." << cpp_name(d);
    }
    stream  << ");";
    stream  << off      << "}\n";

    stream  << off      << "template < typename InputIterator >"
            << off      << "void"
            << off      << "wire_read(InputIterator& begin, InputIterator end, "
                            << qname(struct_) << "& v)"
            << off      << "{";

    stream << mod(+1)   <<      qname(struct_) << " tmp;"
            << off      <<      wire_encoding_read << "(begin, end";
    for (auto d : dm) {
        stream << ", tmp." << cpp_name(d);
    }
    stream << ");"
            << off      <<      "v.swap(tmp);";
    stream << mod(-1)   << "}\n";
}

void
generator::generate_member_read_write( ast::structure_ptr struct_,
        ast::structure_const_ptr parent, bool ovrde)
{
    offset_guard hdr{header_};
    offset_guard src{source_};

    auto const& data_members = struct_->get_data_members();
    ::std::ostringstream qn_os;
    qn_os << qname(struct_);
    auto qn_str = qn_os.str();

    header_ << off      << (( parent || ovrde ) ? "" : "virtual ") << "void"
            << off      << "__wire_write(output_iterator o) const"
                            << (( parent || ovrde ) ? " override" : "") << ";"
            << off      << (( parent || ovrde ) ? "" : "virtual ") << "void"
            << off      << "__wire_read(input_iterator& begin, input_iterator end, "
                            "bool read_head = true)"
                    << (( parent || ovrde ) ? " override" : "") << ";";

    // Source
    //------------------------------------------------------------------------
    //  Wire write member function
    //------------------------------------------------------------------------
    source_ << off << "void"
            << off << qname(struct_)
            << "::__wire_write(output_iterator o) const"
            << off << "{"
            << mod(+1) << "auto encaps = o.encapsulation();";

    auto flags = wire_seg_head_no_flags;
    if (!parent) {
        flags = wire_seg_head_last;
    }

    ::std::string type_id_func = options_.dont_use_hashed_id ? "wire_static_type_id"
            : (qn_str.size() > sizeof(hash_value_type)) ?
                    "wire_static_type_id_hash" : "wire_static_type_id";
    source_ << off << "encaps.start_segment(" << type_id_func << "(), " << flags << ");";

    if (!data_members.empty()) {
        source_ << off << wire_encoding_write << "(o";
        for (auto dm : data_members) {
            source_ << ", " << cpp_name(dm);
        }
        source_ << ");";
    }

    source_ << off << "encaps.end_segment();";

    if (parent) {
        source_ << off << cpp_name(parent) << "::__wire_write(o);";
    }

    source_ << mod(-1) << "}\n";

    //------------------------------------------------------------------------
    //  Wire read member function
    //------------------------------------------------------------------------
    source_ << off << "void"
            << off << qname(struct_)
            << "::__wire_read(input_iterator& begin, input_iterator end, "
                    "bool read_head)"
            << off << "{"
            << mod(+1) << "auto encaps = begin.incoming_encapsulation();"
            << off     << "if (read_head) {"
            << mod(+1) <<    wire_seg_head << " seg_head;"
            << off     <<    "encaps.read_segment_header(begin, end, seg_head);"
            << off     << "::wire::encoding::check_segment_header< " << cpp_name(struct_)
                            << " >(seg_head);"
            << mod(-1) << "}\n";

    if (!data_members.empty()) {
        source_ << off << wire_encoding_read << "(begin, end";
        for (auto dm : data_members) {
            source_ << ", " << cpp_name(dm);
        }
        source_ << ");";
    }

    if (parent) {
        source_ << off << cpp_name(parent) << "::__wire_read(begin, encaps.end(), true);";
    }

    source_ << mod(-1) << "}\n";
}

void
generator::generate_comparison(source_stream& stream, ast::structure_ptr struct_)
{
    auto f = find(struct_->get_annotations(), annotations::GENERATE_CMP);
    if (f == struct_->get_annotations().end())
        return;

    auto const& dm = struct_->get_data_members();
    stream  << off      << "inline bool"
            << off      << "operator == (" << arg_type(struct_) << " lhs, "
                            << arg_type(struct_) << " rhs)"
            << off      << "{";

    if (!dm.empty()) {
        stream << off(+1) << "return ";
        for (auto d = dm.begin(); d != dm.end(); ++d) {
            if (d != dm.begin()) {
                stream << " && " << off(+2);
            }
            stream << "lhs." << cpp_name((*d)) << " == rhs." << cpp_name((*d));
        }
        stream << ";";
    } else {
        stream << off(+1) << "return &lhs == &rhs;";
    }

    stream  << off << "}";

    stream << off       << "inline bool"
            << off      << "operator != (" << arg_type(struct_) << " lhs, "
                            << arg_type(struct_) << " rhs)"
            << off      << "{"
            << off(+1)  <<      "return !(lhs == rhs);"
            << off      << "}";

    stream  << off      << "inline bool"
            << off      << "operator < (" << arg_type(struct_) << " lhs, "
                            << arg_type(struct_) << " rhs)"
            << off      << "{";

    if (!dm.empty()) {
        stream << off(+1) << "return ";
        for (auto d = dm.begin(); d != dm.end(); ++d) {
            if (d != dm.begin()) {
                stream << " && " << off(+2);
            }
            stream << "lhs." << cpp_name((*d)) << " < rhs." << cpp_name((*d));
        }
        stream << ";";
    } else {
        stream << off(+1) << "return &lhs < &rhs;";
    }

    stream << off       << "}\n";
}

void
generator::generate_io(ast::structure_ptr struct_)
{
    offset_guard src{source_};
    auto f = find(struct_->get_annotations(), annotations::GENERATE_IO);
    if (f == struct_->get_annotations().end())
        return;

    auto const& data_members = struct_->get_data_members();
    header_.at_namespace_scope(
    [&](source_stream& stream)
    {
        stream  << off << "::std::ostream&"
                << off << "operator << ( ::std::ostream& os, "
                        << arg_type(struct_) << " val );\n";
    });

    source_ << off << "::std::ostream&"
            << off << "operator << ( ::std::ostream& os, "
                    << arg_type(struct_) << " val )"
            << off << "{";
    {
        if (data_members.empty()) {
            source_ << mod(+1) << "return os;";
        } else {
            offset_guard src{source_};
            source_ << mod(+1) << "::std::ostream::sentry s{os};"
                    << off << "if (s) {"
                    << mod(+1) << "os << '{' ";
            source_.modify_offset(+1);
            for (auto dm = data_members.begin(); dm != data_members.end(); ++dm) {
                if (dm != data_members.begin())
                    source_ << " << \", \"" << off;
                source_ << "<< val." << cpp_name((*dm));
            }
            source_.modify_offset(-1);
            source_ << " << '}';";
            source_ << mod(-1) << "}"
                    << off << "return os;";
        }
    }
    source_ << mod(-1) << "}";
}

::std::string
generator::generate_type_id_funcs(ast::entity_ptr elem)
{
    offset_guard hdr{header_};
    offset_guard src{header_};
    header_ << off(-1)  << "public:"
            << off      <<       "static ::std::string const&"
            << off      <<       "wire_static_type_id();"
            << off      <<       "static " << hash_value_type_name
            << off      <<       "wire_static_type_id_hash();"
    ;

    // Source
    auto eqn = qname(elem);
    auto pfx = constant_prefix(eqn);
    ::std::ostringstream qnos;
    qnos << eqn;
    auto eqn_str = qnos.str();
    source_ << off << "/* data for " << abs_name << eqn
                << " 0x" << ::std::hex << elem->get_hash() << ::std::dec << " */";
    source_ << off << "namespace {\n"
            << mod(+1) << "::std::string const " << pfx << "TYPE_ID = \"" << eqn_str << "\";"
            << off << hash_value_type_name << " const " << pfx << "TYPE_ID_HASH = 0x"
                << ::std::hex << hash::murmur_hash_64(eqn_str) << ::std::dec << ";"
            << "\n"
            << mod(-1) << "} /* namespace for " << abs_name << eqn << " */\n";

    source_ << off << "::std::string const&"
            << off << eqn << "::wire_static_type_id()"
            << off << "{"
            << mod(+1)  << "static ::std::string const type_id_ =\"" << eqn_str << "\";"
            << off      << "return type_id_;"
            << mod(-1)  << "}\n";

    source_ << off << hash_value_type_name
            << off << eqn << "::wire_static_type_id_hash()"
            << off << "{"
            << mod(+1) << "return " << pfx << "TYPE_ID_HASH;"
            << mod(-1) << "}\n";

    return eqn_str;
}

void
generator::generate_wire_functions(ast::interface_ptr iface)
{
    offset_guard hdr{header_};
    offset_guard src{source_};
    auto eqn = qname(iface);

    header_ << off(-1) << "public:";

    header_ << off     << "bool"
            << off     <<   "wire_is_a(::std::string const&,"
            << off(+1) <<         wire_current << " const& = " << wire_no_current << ") const override;\n"
            << off     <<   "::std::string const&"
            << off     <<   "wire_type(" << wire_current << " const& = " << wire_no_current << ") const override;\n"
            << off     <<   "type_list const&"
            << off     <<   "wire_types(" << wire_current << " const& = " << wire_no_current << ") const override;\n"
            << off(-1) << "protected:"
            << off     <<   "bool"
            << off     <<   "__wire_dispatch(" << wire_disp_request << " const&, " << wire_current << " const&,"
            << off(+2) <<           "dispatch_seen_list&, bool throw_not_found) override;"
    ;

    //------------------------------------------------------------------------
    // data
    auto pfx = constant_prefix(eqn);
    {
        auto const& funcs = iface->get_functions();
        source_ << off << "namespace { /*    Type ids and dispatch map for "
                        << abs_name << qname(iface) << "  */\n";
        offset_guard cn{source_};
        source_.modify_offset(+1);
        for (auto f : funcs) {
            source_ << off << "::std::string const " << pfx << cpp_name(f)
                    << " = \"" << cpp_name(f) << "\";";
        }
        for (auto f : funcs) {
            source_ << off << "::std::uint32_t const " << pfx << cpp_name(f) << "_hash"
                    << " = 0x" << ::std::hex << f->get_hash_32() << ";";
        }
        if (!funcs.empty())
            source_ << "\n";
        source_ << off << "using " << pfx << "dispatch_func ="
                << mod(+1) << " void(" << eqn
                << "::*)(" << wire_disp_request << " const&, " << wire_current << " const&);\n";

        // Name dispatch table
        source_ << mod(-1) << "::std::unordered_map< ::std::string, " << pfx << "dispatch_func >"
                << " const " << pfx << "dispatch_map {";

        source_.modify_offset(+1);
        for (auto f : funcs) {
            source_ << off << "{ " << pfx << cpp_name(f) << ", &" << eqn
                    << "::__" << cpp_name(f) << " },";
        }

        source_ << mod(-1) << "}; // dispatch_map\n";

        // Hash dispatch table
        source_ << off << "::std::unordered_map< ::std::uint32_t, " << pfx << "dispatch_func >"
                << " const " << pfx << "hash_dispatch_map {";

        source_.modify_offset(+1);
        for (auto f : funcs) {
            source_ << off << "{ " << pfx << cpp_name(f) << "_hash, &" << eqn
                    << "::__" << cpp_name(f) << " },";
        }

        source_ << mod(-1) << "}; // hash_dispatch_map\n";

        source_ << off << eqn << "::type_list const "
                << pfx << "TYPE_IDS = {";
        source_ << mod(+1) << root_interface << "::wire_static_type_id(),";
        ast::interface_list ancestors;
        iface->collect_ancestors(ancestors);
        for (auto a : ancestors) {
            source_ << off << qname(a) << "::wire_static_type_id(),";
        }
        source_ << off << qname(iface) << "::wire_static_type_id(),"
                << mod(-1) << "};\n";
        source_ << off << "::std::unordered_map< ::std::uint32_t, ::std::string > const "
                        << pfx << "func_names {";
        source_.modify_offset(+1);
        for (auto f : funcs) {
            source_ << off << "{ " << pfx << cpp_name(f) << "_hash, "
                    << pfx << cpp_name(f) << " },";
        }
        source_ << mod(-1) << "}; // func_names";
        source_ << mod(-1) << "} /* namespace */\n";
    }



    //------------------------------------------------------------------------
    // is_a
    source_ << off      << "bool"
            << off      << eqn << "::wire_is_a(::std::string const& name,"
            << mod(+1)  <<      wire_current << " const&) const"
            << mod(-1)  << "{"
            << mod(+1)  <<      "for (auto const& t : " << pfx << "TYPE_IDS) {"
            << mod(+1)  <<          "if (t == name) return true;"
            << mod(-1)  <<      "}"
            << off      <<      "return false;"
            << mod(-1)  << "}";

    //------------------------------------------------------------------------
    // type
    source_ << off      << "::std::string const&"
            << off      << eqn << "::wire_type(" << wire_current <<  " const&) const"
            << off      << "{"
            << mod(+1)  <<      "return wire_static_type_id();"
            << mod(-1)  << "}";

    //------------------------------------------------------------------------
    // types
    source_ << off      << eqn << "::type_list const&"
            << off      << eqn << "::wire_types(" << wire_current << " const&) const"
            << off      << "{"
            << mod(+ 1) <<      "return " << pfx << "TYPE_IDS;"
            << mod(-1)  << "}";

    //------------------------------------------------------------------------
    // the dispatch function
    source_ << off      <<  "bool"
            << off      <<  eqn << "::__wire_dispatch(" << wire_disp_request << " const& req,"
            << mod(+ 1) <<      wire_current << " const& c,"
            << off      <<      "dispatch_seen_list& seen, bool throw_not_found)"
            << mod(-1)  <<  "{"
            << mod(+1)  <<      "if (seen.count(wire_static_type_id_hash())) return false;"
            << off      <<      "seen.insert(wire_static_type_id_hash());"
            << off      <<       pfx << "dispatch_func func = nullptr;"
            << off      <<      "if (c.operation.type() == ::wire::encoding::operation_specs::name_string) {"
            << mod(+1)  <<          "auto f = " << pfx << "dispatch_map.find(c.operation.name());"
            << off      <<          "if (f != " << pfx << "dispatch_map.end()) {"
            << off(+1)  <<              "func = f->second;"
            << off      <<          "}"
            << off(-1)  <<      "} else {"
            << off      <<          "auto op_hash = ::boost::get< ::wire::encoding::operation_specs::hash_type >(c.operation.operation);"
            << off      <<          "auto f = " << pfx << "hash_dispatch_map.find(op_hash);"
            << off      <<          "if (f != " << pfx << "hash_dispatch_map.end()) {"
            << off(+1)  <<              "func = f->second;"
            << off      <<          "}"
            << mod(-1)  <<      "}"
            << off      <<      "if (func) {"
            << off(+1)  <<           "(this->*func)(req, c);"
            << off(+1)  <<           "return true;"
            << off      <<      "}"
            << off      <<      "bool res = ";

    source_.modify_offset(+1);
    auto const& ancestors = iface->get_ancestors();
    if (!ancestors.empty()) {
        for (auto a = ancestors.begin(); a != ancestors.end(); ++a) {
            if (a != ancestors.begin())
                source_ << " ||" << off;
            source_ << qname((*a)) << "::__wire_dispatch(req, c, seen, false)";
        }
    } else {
        source_ << root_interface << "::__wire_dispatch(req, c, seen, false)";
    }
    source_ << ";";
    source_.modify_offset(-1);

    source_ << off      <<      "if (!res && throw_not_found)"
            << mod(+1)  <<          "throw ::wire::errors::no_operation{"
            << mod(+1)  <<              "c.operation.target.identity, c.operation.target.facet, c.operation.operation};"
            << mod(-2)  <<      "return res;"
            << mod(-1) << "}\n";
}

void
generator::generate_exception(ast::exception_ptr exc)
{
    adjust_namespace(exc);
    offset_guard hdr{header_};
    offset_guard src{source_};

    source_ << off << "//" << ::std::setw(77) << ::std::setfill('-') << "-"
            << off << "//    Exception " << abs_name << qname(exc)
            << off << "//" << ::std::setw(77) << ::std::setfill('-') << "-";
    //------------------------------------------------------------------------
    //  Factory initializer for the exception
    //------------------------------------------------------------------------
    source_ << off << "// Factory initializer"
            << off << cpp_name(exc) << "::factory_init_type "
            << cpp_name(exc) << "::_factory_init_;\n";

    auto parent_name = exc->get_parent() ?
            qname(exc->get_parent()) : root_exception;

    header_ << off << "class " << cpp_name(exc)
            << " : public " << cpp_name(parent_name)<< " {";

    auto const& data_members = exc->get_data_members();
    ::std::string qn_str;
    {
        header_.push_scope(exc->name());

        if (!exc->get_types().empty()) {
            header_ << off(-1) << "public:";
            for (auto t : exc->get_types()) {
                generate_type_decl(t);
            }
        }
        if (!exc->get_constants().empty()) {
            header_ << off(-1) << "public:";
            for (auto c : exc->get_constants()) {
                generate_constant(c);
            }
        }

        // Constructors
        header_ << off(-1) << "public:";
        code_snippet members_init{header_.current_scope()};
        if (!data_members.empty()) {
            for (auto dm : data_members) {
                members_init << ", " << cpp_name(dm) << "{}";
            }
        }

        //@{ Default constructor
        header_ << off << "/* default constructor, for use in factories */"
                << off << cpp_name(exc) << "() : " << cpp_name(parent_name) << "{}"
                << members_init << " {}";
        //@}

        header_ << off << "/* templated constructor to format a ::wire::errors::runtime_error message */"
                << off << "template < typename ... T >"
                << off << cpp_name(exc) << "(T const& ... args) : "
                << parent_name << "(args ...)"
                << members_init << "{}";

        // TODO constructors with data members variations
        if (!data_members.empty()) {
            auto current = header_.current_scope();
            code_snippet args{current};
            code_snippet init{current};
            code_snippet msg{current};

            ::std::deque<::std::string> rest;
            for (auto dm : data_members) {
                rest.push_back(dm->name() + "{}"); // FIMXE C++ safe name needed
            }
            for (auto p = data_members.begin(); p != data_members.end(); ++p) {
                if (p != data_members.begin()) {
                    args << ", ";
                    init << "," << off(+1) << "  ";
                    msg << ", ";
                }
                args << arg_type((*p)->get_type()) << " " << cpp_name((*p)) << "_";
                init << cpp_name((*p)) << "{" << cpp_name((*p)) << "_}";
                msg << cpp_name((*p)) << "_";

                rest.pop_front();

                header_ << off      <<  cpp_name(exc) << "(" << args << ")";
                header_ << mod(+1)  <<      ": " << parent_name
                        << "(wire_static_type_id(), " << msg << "), "
                        << off << "  " << init;
                for (auto const& r : rest) {
                    header_ << "," << off << "  " << r;
                }
                header_ << " {}";
                header_.modify_offset(-1);
            }
        }
        header_ << off  << "virtual ~" << cpp_name(exc) << "() = default;";

        if (!data_members.empty()) {
            header_ << off(-1) << "public:";
            for (auto dm : data_members) {
                header_ << off
                        << mapped_type{dm->get_type()} << " " << cpp_name(dm) << ";";
            }
        }

        qn_str = generate_type_id_funcs(exc);
        {
            // Member functions
            generate_member_read_write(exc, exc->get_parent());
            header_ << off     << "::std::exception_ptr"
                    << off     << "make_exception_ptr() override"
                    << off     << "{ return ::std::make_exception_ptr(*this); }";

            plugins_.generate_exception(exc, header_, source_);
        }
        header_ << off(-1)  << "protected:"
                << off      << "void"
                << off      << "stream_message(::std::ostream& os) const override;"
                << off(-1)  << "private:"
                << off      << "using factory_init_type = " << wire_exception_init << "<" << cpp_name(exc) << ">;"
                << off      << "static factory_init_type _factory_init_;";

        source_ << off      << "void"
                << off      << cpp_name(exc) << "::stream_message(::std::ostream& os) const"
                << off      << "{"
                << mod(+1)  << cpp_name(parent_name) << "::stream_message(os);"
                << off      <<      "os << " << cpp_name(exc) << "::wire_static_type_id()";
        if (!data_members.empty()) {
            source_ << " << \":\"";
            for (auto dm : data_members) {
                source_ << " << \" \" << " << cpp_name(dm);
            }
        }
        source_ << ";";
        source_ << mod(-1)  << "}";

        header_ << mod(-1) << "};\n";
        header_.pop_scope();
    }

    // TODO to outer scope
    header_ << off << "using " << cpp_name(exc) << "_ptr = ::std::shared_ptr<"
                << cpp_name(exc) << ">;"
            << off << "using " << cpp_name(exc) << "_weak_ptr = ::std::weak_ptr<"
                << cpp_name(exc) << ">;\n";
}

void
generator::generate_dispatch_interface(ast::interface_ptr iface)
{
    adjust_namespace(iface);
    offset_guard hdr{header_};
    offset_guard src{source_};

    source_ << off      << "//" << ::std::setw(77) << ::std::setfill('-') << "-"
            << off      << "//    Dispatch interface for " << abs_name << qname(iface)
            << off      << "//" << ::std::setw(77) << ::std::setfill('-') << "-";

    header_ << off     << "/**"
            << off     << " *    Dispatch interface for " << abs_name << qname(iface)
            << off     << " */"
            << off     << "class " << cpp_name(iface);

    auto const& ancestors = iface->get_ancestors();
    if (!ancestors.empty()) {
        header_ << mod(+1) << ": ";
        for ( auto a = ancestors.begin(); a != ancestors.end(); ++a ) {
            if (a != ancestors.begin())
                header_ << "," << off << "  ";
            header_ << "public virtual " << qname((*a));
        }
        header_.modify_offset(-1);
    } else {
        header_ << " : public virtual " << root_interface;
    }
    header_ << " {";

    header_.push_scope(iface->name());

    if (!iface->get_types().empty()) {
        header_ << off(-1) << "public:";
        for (auto t : iface->get_types()) {
            generate_type_decl(t);
        }
    }
    if (!iface->get_constants().empty()) {
        header_ << off(-1) << "public:";
        for (auto c : iface->get_constants()) {
            generate_constant(c);
        }
    }
    {
        // Constructor
        header_ << off(-1)  << "public:"
                << off      << cpp_name(iface) << "()"
                << mod(+1)  << ": " << root_interface << "()";
        ast::interface_list anc;

        iface->collect_ancestors(anc, [](ast::entity_const_ptr){ return true; });

        for (auto a : anc) {
            header_ << "," << off << "  "
                    << qname(a) << "()";
        }

        header_ << " {}\n";
        header_.modify_offset(-1);
        header_ << off  << "virtual ~" << cpp_name(iface) << "() = default;";
    }
    auto qn_str = generate_type_id_funcs(iface);
    {
        header_ << off      <<       "static ::std::string const&"
                << off      <<       "wire_function_name(::std::uint32_t);"
                << off      <<       "static bool"
                << off      <<       "wire_find_function(::std::uint32_t hash,"
                << off(+1)  <<           "dispatch_seen_list&, ::std::string const*&);";
    }
    generate_wire_functions(iface);
    auto const& funcs = iface->get_functions();
    if (!funcs.empty()) {
        header_ << off(-1) << "public:";
        // Dispatch methods
        for (auto f : funcs) {
            generate_dispatch_function_member(f);
        }
    }

    source_ << off     << "::std::string const&"
            << off     << qn_str << "::wire_function_name(::std::uint32_t hash)"
            << off     << "{"
            << mod(+1) <<     "::std::string const* str;"
            << off     <<     "dispatch_seen_list seen;"
            << off     <<     "if (wire_find_function(hash, seen, str)) {"
            << off(+1) <<         "return *str;"
            << off     <<     "}"
            << off     <<     "::std::ostringstream os;"
            << off     <<     "os << ::std::hex << hash;"
            << off     <<     "throw ::std::runtime_error{\"No operation with hash 0x\" + os.str()};"
            << mod(-1) << "}\n"
    ;

    auto pfx = constant_prefix(qn_str);
    source_ << off     << "bool"
            << off     << qn_str << "::wire_find_function(::std::uint32_t hash, dispatch_seen_list& seen,"
            << off(+3) <<            "::std::string const*& str)"
            << off     << "{"
            << mod(+1) <<     "if (seen.count(wire_static_type_id_hash()))"
            << off(+1) <<          "return false;"
            << off     <<     "seen.insert(wire_static_type_id_hash());"
            << off     <<     "auto f = " << pfx << "func_names.find(hash);"
            << off     <<     "if (f != " << pfx << "func_names.end()) {"
            << off(+1) <<         "str = &f->second;"
            << off(+1) <<         "return true;"
            << off     <<     "}"
            << off     <<     "bool res = ";
    source_.modify_offset(+1);
    if (!ancestors.empty()) {
        for (auto a = ancestors.begin(); a != ancestors.end(); ++a) {
            if (a != ancestors.begin())
                source_ << " ||" << off;
            source_ << qname((*a)) << "::wire_find_function(hash, seen, str)";
        }
    } else {
        source_ << root_interface << "::wire_find_function(hash, seen, str)";
    }
    source_ << ";";

    source_ << mod(-1) << "return res;"
            << mod(-1) << "}";

    plugins_.generate_dispatch_interface(iface, header_, source_);

    header_ << mod(-1)  << "};\n";
    header_.pop_scope();

}

void
generator::generate_proxy_interface(ast::interface_ptr iface)
{
    offset_guard hdr{header_};
    static const ast::qname base_proxy      {"::wire::core::object_proxy"};
    static const ast::qname ref_ptr         {"::wire::core::reference_ptr"};

    source_ << off      << "//" << ::std::setw(77) << ::std::setfill('-') << "-"
            << off      << "//    Proxy interface for " << abs_name << qname(iface)
            << off      << "//" << ::std::setw(77) << ::std::setfill('-') << "-";

    header_ << off      << "/**"
            << off      << " *    Proxy interface for " << abs_name << qname(iface)
            << off      << " */"
            << off      << "class " << cpp_name(iface) << "_proxy : "
            << off(+1)  <<      "public virtual ::wire::core::proxy< " << cpp_name(iface) << "_proxy";


    auto const& ancestors = iface->get_ancestors();
    if (!ancestors.empty()) {
        header_.modify_offset(+2);
        for ( auto a : ancestors ) {
            auto qn = qname(a);
            qn.components.back() += "_proxy";
            header_ << "," << off << qn;
        }
        header_.modify_offset(-2);
    } else {
        header_ << ", " << base_proxy;
    }
    header_ << "> {"
            << off << "public:";

    header_.push_scope(iface->name() + "_proxy");
    {
        header_ << off << "using interface_type = " << cpp_name(iface) << ";";
    }
    {
        header_ << off(-1)     << "public:";
        offset_guard hdr{header_};
        // TODO Constructors
        header_ << off     << cpp_name(iface) << "_proxy ()"
                << off(+2) << ": " << base_proxy << "{} {}"
                << off     << "explicit"
                << off     << cpp_name(iface) << "_proxy (" << ref_ptr << " _ref, "
                << invocation_opts << " const& _opts = " << invocation_opts <<  "{})"
                << off(+2) << ": " << base_proxy << "{_ref, _opts} {}";
        ;
        header_ << off  << "virtual ~" << cpp_name(iface) << "_proxy() = default;";
    }
    {
        offset_guard hdr{header_};
        header_ << off(-1)  << "public:"
                << off      << "static ::std::string const&"
                << off      << "wire_static_type_id();";

        source_ << off      << "::std::string const&"
                << off      << qname(iface) << "_proxy::wire_static_type_id()"
                << off      << "{"
                << off(+1)  <<      "return " << qname(iface)
                                  << "::wire_static_type_id();"
                << off      << "}\n"
        ;
    }
    auto const& funcs = iface->get_functions();
    if (!funcs.empty()) {
        offset_guard hdr{header_};
        header_ << off(-1)  << "public:";
        for (auto f : funcs) {
            generate_invocation_function_member(f);
        }
    }

    plugins_.generate_proxy_interface(iface, header_, source_);

    header_ << mod(-1)  << "};\n";

    header_.pop_scope();
}

void
generator::generate_interface(ast::interface_ptr iface)
{
    generate_dispatch_interface(iface);
    generate_proxy_interface(iface);

    header_ << off     << "using " << cpp_name(iface)
            << "_ptr = ::std::shared_ptr< " << cpp_name(iface) << " >;";
    header_ << off     << "using " << cpp_name(iface)
            << "_weak_ptr = ::std::weak_ptr< " << cpp_name(iface) << " >;";

    header_ << off     << "using " << cpp_name(iface)
            << "_prx = ::std::shared_ptr< " << cpp_name(iface) << "_proxy >;";
    header_ << "\n";
}

void
generator::generate_class(ast::class_ptr class_)
{
    adjust_namespace(class_);
    offset_guard hdr{header_};
    offset_guard src{header_};
    ast::qname cqn = qname(class_);
    source_ << off      << "//" << ::std::setw(77) << ::std::setfill('-') << "-"
            << off      << "//    Implementation for class " << abs_name << cqn
            << off      << "//" << ::std::setw(77) << ::std::setfill('-') << "-";

    header_ << off      << "/**"
            << off      << " *    Class " << abs_name << cqn
            << off      << " */"
            << off      << "class " << cpp_name(class_);
    auto const& ancestors = class_->get_ancestors();
    auto const& data_members = class_->get_data_members();
    auto parent = class_->get_parent();
    ::std::string qn_str;
    if (parent) {
        header_ << off(+1) << ": public " << qname(parent);
    }
    if (!ancestors.empty()) {
        header_.modify_offset(+1);
        if (parent) {
            header_ << "," << off << "  ";
        } else {
            header_ << off << ": ";
        }
        for (auto a = ancestors.begin(); a != ancestors.end(); ++a) {
            if (a != ancestors.begin())
                header_ << "," << off << "  ";
            header_ << "public virtual " << qname(*a);
        }
        header_.modify_offset(-+11);
    } else if (class_->has_functions()) {
        header_.modify_offset(+1);
        if (parent) {
            header_ << "," << off << "  public virtual " << root_interface;
        } else {
            header_ << off << ": public virtual " << root_interface;
        }
        header_.modify_offset(-1);
    }
    header_ << " {";
    {
        header_.push_scope(class_->name());

        header_ << off(-1) << "public:"
                << off     << "using wire_root_type = "
                << qname(class_->root_class()) << ";";
        if (!parent) {
            header_ << off << "using input_iterator = " << input_iterator_name << ";"
                    << off << "using output_iterator = " << back_inserter
                        << "< " << wire_outgoing << " >;";
        }
        if (!class_->get_types().empty()) {
            header_ << off(-1) << "public:";
            for (auto t : class_->get_types()) {
                generate_type_decl(t);
            }
        }
        if (!class_->get_constants().empty()) {
            header_ << off(-1) << "public:";
            for (auto c : class_->get_constants()) {
                generate_constant(c);
            }
        }

        {
            code_snippet members_init{header_.current_scope()};
            if (!data_members.empty()) {
                for (auto dm = data_members.begin(); dm != data_members.end(); ++dm) {
                    if (dm != data_members.begin())
                        members_init << ", ";
                    members_init << cpp_name((*dm)) << "{}";
                }
            }
            // Constructor
            header_ << off(-1) << "public:";
            header_ << off     <<     cpp_name(class_) << "()";
            bool need_comma {false};
            if (parent) {
                header_ << off(+1) << ": " << qname(parent) << "{}";
                need_comma = true;
            }
            if (class_->is_abstract()) {
                if (need_comma) {
                    header_ << "," << off(+1) << "  ";
                } else {
                    header_ << off(+1) << ": ";
                }
                header_ << root_interface << "{}";
                for (auto a : ancestors) {
                    header_ << "," << off(+1)
                            << "  " << qname(a) << "{}";
                }
                need_comma = true;
            }
            if (!data_members.empty()) {
                if (need_comma) {
                    header_ << "," << off(+1) << "  ";
                } else {
                    header_ << off(+1) << ": ";
                }
                header_ << members_init;
            }

            header_ << " {}";
        }
        {
            // Destuctor
            header_ << off    << "virtual ~" << cpp_name(class_) << "() = default;";
        }
        qn_str = generate_type_id_funcs(class_);
        {
            // Member functions
            generate_member_read_write(class_, parent, false);
        }

        if (class_->is_abstract()) {
            generate_wire_functions(class_);
            auto const& funcs = class_->get_functions();
            if (!funcs.empty()) {
                header_ << off(-1) << "public:";
                // Dispatch methods
                for (auto f : funcs) {
                    generate_dispatch_function_member(f);
                }
                plugins_.generate_dispatch_interface(class_, header_, source_);
            }
        } else {
            // factory initializer
            //------------------------------------------------------------------------
            //  Factory initializer for the class
            //------------------------------------------------------------------------
            source_ << off      << "namespace {"
                    << mod(+1)  << "::wire::encoding::detail::auto_object_factory_init< "
                    << cqn << " > const "
                    << constant_prefix(cqn) << "_factory_init;"
                    << mod(-1) << "}";
        }
        {
            // Data members
            if (!data_members.empty()) {
                header_ << off(-1) << "public:";
                for (auto dm : data_members) {
                    header_ << off
                            << mapped_type{dm->get_type()} << " " << cpp_name(dm) << ";";
                }
            }
        }

        plugins_.generate_class(class_, header_, source_);

        header_ << mod(-1) << "};\n";
    }
    header_.pop_scope();

    //------------------------------------------------------------------------
    //  Type aliases for class pointers
    //------------------------------------------------------------------------
    header_ << off << "using " << cpp_name(class_)
            << "_ptr = ::std::shared_ptr< " << cpp_name(class_) << " >;";
    header_ << off << "using " << cpp_name(class_)
            << "_weak_ptr = ::std::weak_ptr< " << cpp_name(class_) << " >;\n";

    if (class_->has_functions()) {
        generate_proxy_interface(class_);
        header_ << off << "using " << cpp_name(class_)
                << "_prx = ::std::shared_ptr< " << cpp_name(class_) << "_proxy >;";
        header_ << "\n";
    }
}

void
generator::finish_compilation_unit(ast::compilation_unit const& u)
{
    ast::entity_const_set exceptions;
    u.collect_elements(
        exceptions,
        [](ast::entity_const_ptr e)
        {
            return (bool)ast::dynamic_entity_cast< ast::exception >(e).get();
        });
    if (!exceptions.empty()) {
        header_.adjust_namespace(wire_encoding_detail);
        for (auto ex : exceptions) {
            header_ << off << "template <>"
                    << off << "struct is_user_exception< " << qname(ex)
                        << " > : ::std::true_type {};";
        }
        header_ << "\n";
    }
    ast::entity_const_set interfaces;
    u.collect_elements(
        interfaces,
        [](ast::entity_const_ptr e)
        {
            auto iface = ast::dynamic_entity_cast< ast::interface >(e);
            if (iface) {
                auto cls = ast::dynamic_entity_cast<ast::class_>(iface);
                return !cls || cls->has_functions();
            }
            return false;
        });
    if (!interfaces.empty()) {
        header_.adjust_namespace(wire_encoding_detail);
        for (auto iface : interfaces) {
            header_ << off << "template <>"
                    << off << "struct is_proxy< " << qname(iface) << "_proxy"
                        << " >: ::std::true_type {};";
        }
        header_ << "\n";
    }
    ast::entity_const_set enums;
    u.collect_elements(enums,
        [](ast::entity_const_ptr e)
        {
            return (bool)ast::dynamic_entity_cast< ast::enumeration >(e).get();
        });
    if (!enums.empty()) {
        header_.adjust_namespace(wire_util_namespace);
        for (auto e : enums) {
            generate_enum_traits(ast::dynamic_entity_cast< ast::enumeration >(e));
        }
    }

    plugins_.finish_compilation_unit(u, header_, source_);
}

}  /* namespace cpp */
}  /* namespace idl */
}  /* namespace wire */
