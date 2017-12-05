// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_CAPNP_COMMON_TYPES_H
#define BITCOIN_INTERFACES_CAPNP_COMMON_TYPES_H

#include <chainparams.h>
#include <interfaces/capnp/chain.capnp.proxy.h>
#include <interfaces/capnp/common.capnp.proxy.h>
#include <interfaces/capnp/handler.capnp.proxy.h>
#include <interfaces/capnp/init.capnp.proxy.h>
#include <interfaces/capnp/node.capnp.proxy.h>
#include <interfaces/capnp/wallet.capnp.proxy.h>
#include <mp/proxy-types.h>
#include <net_processing.h>
#include <netbase.h>
#include <validation.h>
#include <wallet/coincontrol.h>

#include <consensus/validation.h>

namespace interfaces {
namespace capnp {

//! Convert kj::StringPtr to std::string.
inline std::string ToString(const kj::StringPtr& str) { return {str.cStr(), str.size()}; }

//! Convert kj::ArrayPtr to std::string.
inline std::string ToString(const kj::ArrayPtr<const kj::byte>& data)
{
    return {reinterpret_cast<const char*>(data.begin()), data.size()};
}

//! Convert array object to kj::ArrayPtr.
template <typename Array>
inline kj::ArrayPtr<const kj::byte> ToArray(const Array& array)
{
    return {reinterpret_cast<const kj::byte*>(array.data()), array.size()};
}

//! Convert base_blob to kj::ArrayPtr.
template <typename Blob>
inline kj::ArrayPtr<const kj::byte> FromBlob(const Blob& blob)
{
    return {blob.begin(), blob.size()};
}

//! Convert kj::ArrayPtr to base_blob
template <typename Blob>
inline Blob ToBlob(kj::ArrayPtr<const kj::byte> data)
{
    // TODO: Avoid temp vector.
    return Blob(std::vector<unsigned char>(data.begin(), data.begin() + data.size()));
}

//! Serialize bitcoin value.
template <typename T>
CDataStream Serialize(const T& value)
{
    CDataStream stream(SER_NETWORK, CLIENT_VERSION);
    value.Serialize(stream);
    return stream;
}

//! Deserialize bitcoin value.
template <typename T>
T Unserialize(T& value, const kj::ArrayPtr<const kj::byte>& data)
{
    // Could optimize, it unnecessarily copies the data into a temporary vector.
    CDataStream stream(reinterpret_cast<const char*>(data.begin()), reinterpret_cast<const char*>(data.end()),
        SER_NETWORK, CLIENT_VERSION);
    value.Unserialize(stream);
    return value;
}

//! Deserialize bitcoin value.
template <typename T>
T Unserialize(const kj::ArrayPtr<const kj::byte>& data)
{
    T value;
    Unserialize(value, data);
    return value;
}

template <typename T>
using Deserializable = std::is_constructible<T, ::deserialize_type, ::CDataStream&>;

template <typename T>
struct Unserializable
{
private:
    template <typename C>
    static std::true_type test(decltype(std::declval<C>().Unserialize(std::declval<C&>()))*);
    template <typename>
    static std::false_type test(...);

public:
    static constexpr bool value = decltype(test<T>(nullptr))::value;
};

template <typename T>
struct Serializable
{
private:
    template <typename C>
    static std::true_type test(decltype(std::declval<C>().Serialize(std::declval<C&>()))*);
    template <typename>
    static std::false_type test(...);

public:
    static constexpr bool value = decltype(test<T>(nullptr))::value;
};

} // namespace capnp
} // namespace interfaces

namespace mp {
//!@{
//! Functions to serialize / deserialize bitcoin objects that don't
//! already provide their own serialization.
void CustomBuildMessage(InvokeContext& invoke_context,
    const UniValue& univalue,
    interfaces::capnp::messages::UniValue::Builder&& builder);
void CustomReadMessage(InvokeContext& invoke_context,
    const interfaces::capnp::messages::UniValue::Reader& reader,
    UniValue& univalue);
//!@}

template <typename LocalType, typename Reader, typename ReadDest>
decltype(auto) CustomReadField(TypeList<LocalType>, Priority<1>,
    InvokeContext& invoke_context,
    Reader&& reader,
    ReadDest&& read_dest,
    decltype(CustomReadMessage(invoke_context, reader.get(), std::declval<LocalType&>()))* enable = nullptr)
{
    return read_dest.update([&](auto& value) {
    CustomReadMessage(invoke_context, reader.get(), value);
    });
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<LocalType>, Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest,
    typename std::enable_if<interfaces::capnp::Deserializable<LocalType>::value>::type* enable = nullptr)
{
    if (!input.has()) return read_dest.construct();
    auto data = input.get();
    // Note: stream copy here is unnecessary, and can be avoided in the future
    // when `VectorReader` from #12254 is added.
    CDataStream stream(CharCast(data.begin()), CharCast(data.end()), SER_NETWORK, CLIENT_VERSION);
    return read_dest.construct(deserialize, stream);
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<LocalType>, Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest,
    // FIXME instead of always preferring Deserialize implementation over Unserialize should prefer Deserializing when emplacing, unserialize when updating
    typename std::enable_if<
                   interfaces::capnp::Unserializable<LocalType>::value && !
                   interfaces::capnp::Deserializable<LocalType>::value>::type* enable = nullptr)
{
    return read_dest.update([&](auto& value) {
    if (!input.has()) return;
    auto data = input.get();
    // Note: stream copy here is unnecessary, and can be avoided in the future
    // when `VectorReader` from #12254 is added.
    CDataStream stream(CharCast(data.begin()), CharCast(data.end()), SER_NETWORK, CLIENT_VERSION);
    value.Unserialize(stream);
    });
}

template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<SecureString>, Priority<1>, InvokeContext& invoke_context, Input&& input, ReadDest&& read_dest)
{
    auto data = input.get();
    // Copy input into SecureString. Caller needs to be responsible for calling
    // memory_cleanse on the input.
    return read_dest.construct(CharCast(data.begin()), data.size());
}

template <typename Value, typename Output>
void CustomBuildField(TypeList<SecureString>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& str,
    Output&& output)
{
    auto result = output.init(str.size());
    // Copy SecureString into output. Caller needs to be responsible for calling
    // memory_cleanse later on the output after it is sent.
    memcpy(result.begin(), str.data(), str.size());
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<LocalType>,
    Priority<2>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output,
    decltype(CustomBuildMessage(invoke_context, value, output.init()))* enable = nullptr)
{
    CustomBuildMessage(invoke_context, value, output.init());
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<LocalType>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output,
    typename std::enable_if<interfaces::capnp::Serializable<
        typename std::remove_cv<typename std::remove_reference<Value>::type>::type>::value>::type* enable = nullptr)
{
    CDataStream stream(SER_NETWORK, CLIENT_VERSION);
    value.Serialize(stream);
    auto result = output.init(stream.size());
    memcpy(result.begin(), stream.data(), stream.size());
}

template <typename Accessor, typename ServerContext, typename Fn, typename... Args>
auto CustomPassField(TypeList<>, ServerContext& server_context, const Fn& fn, Args&&... args) ->
    typename std::enable_if<std::is_same<decltype(Accessor::get(server_context.call_context.getParams())),
        interfaces::capnp::messages::GlobalArgs::Reader>::value>::type
{
    interfaces::capnp::ReadGlobalArgs(server_context, Accessor::get(server_context.call_context.getParams()));
    return fn.invoke(server_context, std::forward<Args>(args)...);
}

template <typename Output>
void CustomBuildField(TypeList<>,
    Priority<1>,
    InvokeContext& invoke_context,
    Output&& output,
    typename std::enable_if<std::is_same<decltype(output.init()),
        interfaces::capnp::messages::GlobalArgs::Builder>::value>::type* enable = nullptr)
{
    interfaces::capnp::BuildGlobalArgs(invoke_context, output.init());
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<Optional<LocalType>>, Priority<1>, InvokeContext& invoke_context, Input&& input, ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
    if (!input.has()) {
        value.reset();
        return;
    }
    if (value) {
        ReadField(TypeList<LocalType>(), invoke_context, input, ReadDestValue(*value));
    } else {
        ReadField(TypeList<LocalType>(), invoke_context, input,
            ReadDestEmplace(TypeList<LocalType>(), [&](auto&&... args) -> auto& {
                value.emplace(std::forward<decltype(args)>(args)...);
                return *value;
            }));
    }
    });
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<Optional<LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    if (value) {
        output.setHas();
        BuildField(TypeList<LocalType>(), invoke_context, output, *value);
    }
}

} // namespace mp

#endif // BITCOIN_INTERFACES_CAPNP_COMMON_TYPES_H
