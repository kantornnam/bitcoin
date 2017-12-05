#include <algorithm>
#include <boost/core/explicit_operator_bool.hpp>
#include <boost/optional/optional.hpp>
#include <capnp/blob.h>
#include <capnp/schema-parser.h>
#include <capnp/schema.capnp.h>
#include <capnp/schema.h>
#include <cctype>
#include <fstream>
#include <interfaces/capnp/proxy.capnp.h>
#include <kj/common.h>
#include <kj/string.h>
#include <map>
#include <memory>
#include <sstream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <capnp/schema-lite.h>
#include <iostream>

#define PROXY_BIN "interfaces/capnp/proxy-codegen"
#define PROXY_DECL "interfaces/capnp/proxy.h"
#define PROXY_IMPL "interfaces/capnp/proxy-impl.h"

constexpr uint64_t NAMESPACE_ANNOTATION_ID = 0xb9c6f99ebf805f2cull; // From c++.capnp
constexpr uint64_t PROXY_ANNOTATION_ID = 0xbaf188aa5b50aacfull;     // From proxy.capnp
constexpr uint64_t COUNT_ANNOTATION_ID = 0xd02682b319f69b38ull;     // From proxy.capnp
constexpr uint64_t EXCEPTION_ANNOTATION_ID = 0x996a183200992f88ull; // From proxy.capnp
constexpr uint64_t NAME_ANNOTATION_ID = 0xb594888f63f4dbb9ull;      // From proxy.capnp
constexpr uint64_t SKIP_ANNOTATION_ID = 0x824c08b82695d8ddull;      // From proxy.capnp

template <typename Reader>
boost::optional<capnp::schema::Value::Reader> GetAnnotation(const Reader& reader, uint64_t id)
{
    for (const auto& annotation : reader.getAnnotations()) {
        if (annotation.getId() == id) return annotation.getValue();
    }
    return {};
}

using CharSlice = kj::ArrayPtr<const char>;

// Overload for any type with a string .begin(), like kj::StringPtr and kj::ArrayPtr<char>.
template <class OutputStream, class Array, const char* Enable = decltype(std::declval<Array>().begin())()>
OutputStream& operator<<(OutputStream& os, const Array& array)
{
    os.write(array.begin(), array.size());
    return os;
}

struct Format
{
    template <typename Value>
    Format& operator<<(Value&& value)
    {
        m_os << value;
        return *this;
    }
    operator std::string() { return m_os.str(); }
    std::ostringstream m_os;
};

std::string Cap(kj::StringPtr str)
{
    std::string result = str;
    if (!result.empty()) result[0] = toupper(result[0]);
    return result;
}

bool PrimitiveType(const ::capnp::Type& type)
{
    return type.isVoid() || type.isBool() || type.isInt8() || type.isInt16() || type.isInt32() || type.isInt64() ||
           type.isUInt8() || type.isUInt16() || type.isUInt32() || type.isUInt64() || type.isFloat32() ||
           type.isFloat64();
}

bool InitType(const ::capnp::Type& type) { return !PrimitiveType(type) && !type.isInterface(); }

void PrintType(std::ostream& os, kj::StringPtr message_namespace, const ::capnp::Type& type);

void PrintBrand(std::ostream& os, kj::StringPtr message_namespace, const ::capnp::Schema& schema)
{
    if (schema.getProto().getIsGeneric()) {
        os << "<";
        bool first = true;
        for (const auto& arg : schema.getBrandArgumentsAtScope(schema.getProto().getId())) {
            if (first)
                first = false;
            else
                os << ", ";
            PrintType(os, message_namespace, arg);
        }
        os << ">";
    }
}

void PrintType(std::ostream& os, kj::StringPtr message_namespace, const ::capnp::Type& type)
{
    switch (type.which()) {
    case ::capnp::schema::Type::VOID:
        os << "::capnp::Void";
        break;
    case ::capnp::schema::Type::BOOL:
        os << "bool";
        break;
    case ::capnp::schema::Type::INT8:
        os << "int8_t";
        break;
    case ::capnp::schema::Type::INT16:
        os << "int16_t";
        break;
    case ::capnp::schema::Type::INT32:
        os << "int32_t";
        break;
    case ::capnp::schema::Type::INT64:
        os << "int64_t";
        break;
    case ::capnp::schema::Type::UINT8:
        os << "uint8_t";
        break;
    case ::capnp::schema::Type::UINT16:
        os << "uint16_t";
        break;
    case ::capnp::schema::Type::UINT32:
        os << "uint32_t";
        break;
    case ::capnp::schema::Type::UINT64:
        os << "uint64_t";
        break;
    case ::capnp::schema::Type::FLOAT32:
        os << "float";
        break;
    case ::capnp::schema::Type::FLOAT64:
        os << "double";
        break;
    case ::capnp::schema::Type::TEXT:
        os << "::capnp::Text";
        break;
    case ::capnp::schema::Type::DATA:
        os << "::capnp::Data";
        break;
    case ::capnp::schema::Type::LIST:
        os << "::capnp::List<";
        PrintType(os, message_namespace, type.asList().getElementType());
        os << ">";
        break;
    case ::capnp::schema::Type::ENUM:
        os << message_namespace << "::" << type.asEnum().getShortDisplayName();
        break;
    case ::capnp::schema::Type::STRUCT:
        os << message_namespace << "::" << type.asStruct().getShortDisplayName();
        PrintBrand(os, message_namespace, type.asStruct());
        break;
    case ::capnp::schema::Type::INTERFACE:
        os << message_namespace << "::" << type.asInterface().getShortDisplayName();
        break;
    case ::capnp::schema::Type::ANY_POINTER:
        os << "::capnp::AnyPointer";
        break;
    }
}

void Generate(kj::StringPtr input_schema, kj::StringPtr import_path, kj::StringPtr output_stem)
{
    capnp::SchemaParser parser;
    auto file_schema = parser.parseDiskFile(input_schema, input_schema, {import_path});

    const std::string stem = output_stem;
    std::ofstream cpp(stem + ".capnp.proxy.c++");
    cpp << "// Generated by " PROXY_BIN " from " << input_schema << "\n\n";
    cpp << "#include <" << stem << ".capnp.proxy-impl.h>\n";
    cpp << "#include <" << PROXY_IMPL << ">\n\n";
    cpp << "namespace interfaces {\n";
    cpp << "namespace capnp {\n";

    std::string guard = stem;
    std::transform(guard.begin(), guard.end(), guard.begin(),
        [](unsigned char c) { return std::isalnum(c) ? std::toupper(c) : '_'; });

    std::ofstream impl(stem + ".capnp.proxy-impl.h");
    impl << "// Generated by " PROXY_BIN " from " << input_schema << "\n\n";
    impl << "#ifndef " << guard << "_CAPNP_PROXY_IMPL_H\n";
    impl << "#define " << guard << "_CAPNP_PROXY_IMPL_H\n\n";
    impl << "#include <" << stem << ".capnp.proxy.h>\n";
    impl << "#include <" << stem << "-impl.h>\n\n";
    impl << "namespace interfaces {\n";
    impl << "namespace capnp {\n";

    std::ofstream h(stem + ".capnp.proxy.h");
    h << "// Generated by " PROXY_BIN " from " << input_schema << "\n\n";
    h << "#ifndef " << guard << "_CAPNP_PROXY_H\n";
    h << "#define " << guard << "_CAPNP_PROXY_H\n\n";
    h << "#include <" << stem << ".h>\n";
    h << "#include <" << PROXY_DECL << ">\n\n";
    h << "namespace interfaces {\n";
    h << "namespace capnp {\n";

    kj::StringPtr message_namespace;
    if (auto value = GetAnnotation(file_schema.getProto(), NAMESPACE_ANNOTATION_ID)) {
        message_namespace = value->getText();
    }

    auto print_setter = [&](std::ostream& os, const std::string& builder, const ::capnp::StructSchema::Field& field,
                            const ::capnp::schema::Node::Reader* r = nullptr) {
        bool cast = field.getType().isAnyPointer() || field.getType().isInterface();
        if (cast) os << "static_cast<";
        if (field.getType().isAnyPointer()) {
            auto ap = field.getProto().getSlot().getType().getAnyPointer().getParameter();
            assert(ap.getScopeId() == r->getId());
            os << "typename CapTypeTraits<" << r->getParameters()[ap.getParameterIndex()].getName()
               << ">::template Setter<typename " << builder << ">";
        } else if (field.getType().isInterface()) {
            const auto& interface = field.getType().asInterface();
            os << "void (" << builder << "::*)(" << message_namespace << "::" << interface.getShortDisplayName()
               << "::Client&&)";
        }
        if (cast) os << ">(";
        os << "&" << builder << "::";
        os << (InitType(field.getType()) ? "init" : "set");
        os << Cap(field.getProto().getName());
        if (cast) os << ")";
    };

    for (const auto& node_nested : file_schema.getProto().getNestedNodes()) {
        kj::StringPtr node_name = node_nested.getName();
        const auto& node = file_schema.getNested(node_name);
        kj::StringPtr proxied_class_type;
        if (auto proxy = GetAnnotation(node.getProto(), PROXY_ANNOTATION_ID)) {
            proxied_class_type = proxy->getText();
        }

        if (node.getProto().isStruct()) {
            const auto& struc = node.asStruct();
            std::ostringstream generic_name;
            generic_name << node_name;
            h << "template<";
            bool first_param = true;
            for (const auto& param : node.getProto().getParameters()) {
                if (first_param) {
                    first_param = false;
                    generic_name << "<";
                } else {
                    h << ", ";
                    generic_name << ", ";
                }
                h << "typename " << param.getName();
                generic_name << "" << param.getName();
            }
            if (!first_param) generic_name << ">";
            h << ">\n";
            h << "struct ProxyStruct<" << message_namespace << "::" << generic_name.str() << ">\n";
            h << "{\n";
            h << "    using Struct = " << message_namespace << "::" << generic_name.str() << ";\n";
            size_t i = 0;
            for (const auto& field : struc.getFields()) {
                auto field_name = field.getProto().getName();
                h << "    static auto get" << Cap(field_name) << "() -> AUTO_RETURN((Make<Accessor, ";
                if (field.getType().which() ==
                    ::capnp::schema::Type::STRUCT) { // FIXME workaround broken message_namespace
                    h << "typename decltype(std::declval<Struct::Reader>().get" << Cap(field_name) << "())::Reads";
                } else {
                    PrintType(h, message_namespace, field.getType());
                }
                h << ">(&Struct::Reader::get" << Cap(field_name) << ", ";
                auto n = node.getProto();
                print_setter(h, "Struct::Builder", field, &n);
                h << ", ";
                if (!PrimitiveType(field.getType())) {
                    h << "&Struct::Reader::has" << Cap(field_name);
                } else {
                    h << "nullptr";
                }
                h << ", nullptr, nullptr, nullptr)))\n";

                if (GetAnnotation(field.getProto(), SKIP_ANNOTATION_ID)) {
                    continue;
                }
                h << "    static auto get(std::integral_constant<size_t, " << i << ">) -> AUTO_RETURN(get"
                  << Cap(field_name) << "())\n";
                ++i;
            }
            h << "    static constexpr size_t fields = " << i << ";\n";
            h << "};\n";

            if (proxied_class_type.size()) {
                impl << "template<>\n";
                impl << "struct ProxyType<" << proxied_class_type << ">\n";
                impl << "{\n";
                impl << "public:\n";
                impl << "    using Struct = " << message_namespace << "::" << node_name << ";\n";
                size_t i = 0;
                for (const auto& field : struc.getFields()) {
                    if (GetAnnotation(field.getProto(), SKIP_ANNOTATION_ID)) {
                        continue;
                    }
                    auto field_name = field.getProto().getName();
                    auto member_name = field_name;
                    if (auto name = GetAnnotation(field.getProto(), NAME_ANNOTATION_ID)) {
                        member_name = name->getText();
                    }
                    impl << "    static auto get(std::integral_constant<size_t, " << i << ">) -> AUTO_RETURN("
                         << "&" << proxied_class_type << "::" << member_name << ")\n";
                    ++i;
                }
                impl << "    static constexpr size_t fields = " << i << ";\n";
                impl << "};\n";
            }
        }

        if (proxied_class_type.size() && node.getProto().isInterface()) {
            const auto& interface = node.asInterface();

            std::ostringstream client;
            client << "template<>\nstruct ProxyClient<" << message_namespace << "::" << node_name << "> : ";
            client << "public ProxyClientCustom<" << message_namespace << "::" << node_name << ", "
                   << proxied_class_type << "> {\n";
            client << "public:\n";
            client << "    using ProxyClientCustom::ProxyClientCustom;\n";
            client << "    ~ProxyClient();\n";

            std::ostringstream server;
            server << "template<>\nstruct ProxyServer<" << message_namespace << "::" << node_name << "> : public "
                   << "ProxyServerCustom<" << message_namespace << "::" << node_name << ", " << proxied_class_type
                   << ">";
            server << "\n{\npublic:\n";
            server << "    using ProxyServerCustom::ProxyServerCustom;\n";
            server << "    ~ProxyServer();\n";

            std::ostringstream client_destroy;
            std::ostringstream methods;

            for (const auto& method : interface.getMethods()) {
                kj::StringPtr method_name = method.getProto().getName();
                kj::StringPtr proxied_method_name = method_name;
                if (auto name = GetAnnotation(method.getProto(), NAME_ANNOTATION_ID)) {
                    proxied_method_name = name->getText();
                }

                const std::string method_prefix = Format() << message_namespace << "::" << node_name
                                                           << "::" << Cap(method_name);
                bool is_destroy = method_name == "destroy";

                struct Field
                {
                    boost::optional<::capnp::StructSchema::Field> param;
                    boost::optional<::capnp::StructSchema::Field> result;
                    int args = 0;
                    bool retval = false;
                    bool has = false;
                    bool want = false;
                    bool skip = false;
                    kj::StringPtr exception;

                    bool hasHas() const
                    {
                        return (param && !PrimitiveType(param->getType())) ||
                               ((result && !PrimitiveType(result->getType())));
                    }
                };

                std::vector<Field> fields;
                std::map<kj::StringPtr, int> field_idx; // name -> args index
                bool has_result = false;

                auto add_field = [&](const ::capnp::StructSchema::Field& schema_field, bool param) {
                    auto field_name = schema_field.getProto().getName();
                    auto inserted = field_idx.emplace(field_name, fields.size());
                    if (inserted.second) {
                        fields.emplace_back();
                    }
                    auto& field = fields[inserted.first->second];
                    (param ? field.param : field.result) = schema_field;

                    if (!param && field_name == "result") {
                        field.retval = true;
                        has_result = true;
                    }

                    if (auto value = GetAnnotation(schema_field.getProto(), EXCEPTION_ANNOTATION_ID)) {
                        field.exception = value->getText();
                    }

                    boost::optional<int> count;
                    if (auto value = GetAnnotation(schema_field.getProto(), COUNT_ANNOTATION_ID)) {
                        count = value->getInt32();
                    } else if (schema_field.getType().isStruct()) {
                        if (auto value =
                                GetAnnotation(schema_field.getType().asStruct().getProto(), COUNT_ANNOTATION_ID)) {
                            count = value->getInt32();
                        }
                    }

                    if (inserted.second && !field.retval && !field.exception.size()) {
                        if (count) {
                            field.args = *count;
                        } else {
                            field.args = 1;
                        }
                    }
                };

                for (const auto& schema_field : method.getParamType().getFields()) {
                    add_field(schema_field, true);
                }
                for (const auto& schema_field : method.getResultType().getFields()) {
                    add_field(schema_field, false);
                }
                for (auto& field : field_idx) {
                    auto has_field = field_idx.find("has" + Cap(field.first));
                    if (has_field != field_idx.end() && !fields[field.second].hasHas()) {
                        fields[has_field->second].skip = true;
                        fields[field.second].has = true;
                    }
                    auto want_field = field_idx.find("want" + Cap(field.first));
                    if (want_field != field_idx.end() && !fields[want_field->second].result) {
                        fields[want_field->second].skip = true;
                        fields[field.second].want = true;
                    }
                }

                auto print_accessor = [&](std::ostream& os, bool client, const Field& field) {
                    auto& input = client ? field.result : field.param;
                    auto& output = client ? field.param : field.result;
                    std::string input_reader = Format()
                                               << method_prefix << (client ? "Results" : "Params") << "::Reader";
                    std::string output_builder = Format()
                                                 << method_prefix << (client ? "Params" : "Results") << "::Builder";
                    auto field_suffix = Cap(input ? input->getProto().getName() : output->getProto().getName());
                    auto field_type = input ? input->getType() : output->getType();

                    os << "Make<Accessor, ";
                    if (field_type.which() ==
                        ::capnp::schema::Type::STRUCT) { // FIXME workaround broken message_namespace
                        os << "typename decltype(std::declval<" << method_prefix
                           << (field.param ? "Params" : "Results") << "::Reader>().get" << field_suffix
                           << "())::Reads";
                    } else {
                        PrintType(os, message_namespace, field_type);
                    }
                    os << ">(";
                    if (input) {
                        os << "&" << input_reader << "::get" << field_suffix;
                    } else {
                        os << "nullptr";
                    }
                    os << ", ";

                    if (output) {
                        print_setter(os, output_builder, *output);
                    } else {
                        os << "nullptr";
                    }
                    os << ", ";
                    if (input && field.has) {
                        os << "&" << input_reader << "::getHas" << field_suffix;
                    } else if (input && field.hasHas()) {
                        os << "&" << input_reader << "::has" << field_suffix;
                    } else {
                        os << "nullptr";
                    }
                    os << ", ";
                    if (output && field.has) {
                        os << "&" << output_builder << "::setHas" << field_suffix;
                    } else {
                        os << "nullptr";
                    }
                    os << ", ";
                    if (field.want) {
                        os << "&" << method_prefix << "Params::Reader::getWant" << field_suffix << ", ";
                        os << "&" << method_prefix << "Params::Builder::setWant" << field_suffix;
                    } else {
                        os << "nullptr, nullptr";
                    }
                    os << ")";
                };

                if (!is_destroy) {
                    methods << "template<>\n";
                    methods << "struct ProxyMethod<" << method_prefix << "Params>\n";
                    methods << "{\n";
                    methods << "    static constexpr auto method = &" << proxied_class_type
                            << "::" << proxied_method_name << ";\n";
                    methods << "};\n\n";
                }

                std::ostringstream client_args;
                std::ostringstream client_invoke;
                std::ostringstream server_invoke_start;
                std::ostringstream server_invoke_end;
                int argc = 0;
                client_invoke << "&" << message_namespace << "::" << node_name << "::Client::" << method_name
                              << "Request, *this";
                for (const auto& field : fields) {
                    if (field.skip) continue;

                    auto field_name = field.param ? field.param->getProto().getName() :
                                                    field.result ? field.result->getProto().getName() : "";

                    for (int i = 0; i < field.args; ++i) {
                        if (argc > 0) client_args << ",";
                        client_args << "M" << method.getOrdinal() << "::Param<" << argc << "> " << field_name;
                        if (field.args > 1) client_args << i;
                        ++argc;
                    }
                    client_invoke << ", ";

                    if (field.exception.size()) {
                        client_invoke << "MakeClientException<" << field.exception << ">(";
                    } else {
                        client_invoke << "MakeClientParam<" << argc << "/*DEBUG*/>(";
                    }
                    print_accessor(client_invoke, true, field);
                    if (field.retval || field.args == 1) {
                        client_invoke << ", " << field_name;
                    } else {
                        for (int i = 0; i < field.args; ++i) {
                            client_invoke << ", " << field_name << i;
                        }
                    }
                    client_invoke << ")";

                    if (field.exception.size()) {
                        server_invoke_start << "Make<ServerExcept, " << field.exception << ">";
                    } else if (field.retval) {
                        server_invoke_start << "Make<ServerRet>";
                    } else {
                        server_invoke_start << "MakeServerField<" << field.args << ">";
                    }
                    server_invoke_start << "(";
                    print_accessor(server_invoke_start, false, field);
                    server_invoke_start << ", ";
                    server_invoke_end << ")";
                }

                if (is_destroy) {
                    client_destroy << "clientInvoke(TypeList<>(), " << client_invoke.str() << ");";
                } else {
                    client << "    using M" << method.getOrdinal() << " = ProxyMethodTraits<" << method_prefix
                           << "Params>;\n";
                    client << "    typename M" << method.getOrdinal() << "::Result " << method_name << "("
                           << client_args.str() << ")";
                    client << ";\n";
                    cpp << "ProxyClient<" << message_namespace << "::" << node_name << ">::M" << method.getOrdinal()
                        << "::Result ProxyClient<" << message_namespace << "::" << node_name << ">::" << method_name
                        << "(" << client_args.str() << ") {\n";
                    if (has_result) {
                        // FIXME: Invoke function should just return the result directly to simplify proxy gen and get
                        // rid of need for this variable. This would also unify server & client implementations.
                        cpp << "    typename M" << method.getOrdinal() << "::Result result;\n";
                    }
                    cpp << "    clientInvoke(typename M" << method.getOrdinal() << "::Fields(), "
                        << client_invoke.str() << ");\n";
                    if (has_result) cpp << "    return result;\n";
                    cpp << "}\n";
                }

                server << "    kj::Promise<void> " << method_name << "(" << Cap(method_name)
                       << "Context method_context) override;\n";
                cpp << "kj::Promise<void> ProxyServer<" << message_namespace << "::" << node_name
                    << ">::" << method_name << "(" << Cap(method_name)
                    << "Context method_context) {\n"
                       "    return serverInvoke(*this, method_context, "
                    << server_invoke_start.str();
                if (is_destroy) {
                    cpp << "ServerDestroy()";
                } else {
                    cpp << "MakeServerMethod<" << method.getOrdinal() << ">(&" << proxied_class_type
                        << "::" << proxied_method_name << ")";
                }
                cpp << server_invoke_end.str() << ");\n}\n";
            }

            client << "};\n";
            server << "};\n";
            h << "\n" << methods.str() << client.str() << "\n" << server.str() << "\n";
            cpp << "ProxyClient<" << message_namespace << "::" << node_name
                << ">::~ProxyClient() { clientDestroy(*this); " << client_destroy.str() << " }\n";
            cpp << "ProxyServer<" << message_namespace << "::" << node_name
                << ">::~ProxyServer() { serverDestroy(*this); }\n";
        }
    }

    cpp << "} // namespace capnp\n";
    cpp << "} // namespace interfaces\n";

    impl << "} // namespace capnp\n";
    impl << "} // namespace interfaces\n";
    impl << "#endif\n";

    h << "} // namespace capnp\n";
    h << "} // namespace interfaces\n";
    h << "#endif\n";
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: " PROXY_BIN " INPUT_SCHEMA IMPORT_PATH OUTPUT_STEM\n");
        exit(1);
    }
    Generate(argv[1], argv[2], argv[3]);
    return 0;
}
