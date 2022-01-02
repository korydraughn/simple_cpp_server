#include "message_generated.h"

#include <boost/asio.hpp>
#include <boost/endian/buffers.hpp>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <iostream>
#include <string>

int main(int _argc, char* _argv[])
{
    if (_argc != 3) {
        fmt::print(stderr, "Usage: fbs_client <port> <message>\n");
        return 1;
    }

    try {
        using boost::asio::ip::tcp;

        tcp::iostream s{"localhost", _argv[1]};
        if (!s) {
            fmt::print(stderr, "Unable to connect: {}\n", s.error().message());
            return 1;
        }

        namespace fbs = flatbuffers;
        namespace kdd = kdd::scpps;

        fbs::FlatBufferBuilder builder{1024};

        // FlatBuffers requires that nested data be created first.
        // Hence, the creation of strings here.
        auto username = builder.CreateString("kory");
        auto proxy_username = builder.CreateString("rods");
        auto payload = builder.CreateString(_argv[2]);

        kdd::user_infoBuilder user_builder{builder};
        user_builder.add_name(username);
        auto user = user_builder.Finish();

        kdd::user_infoBuilder proxy_user_builder{builder};
        proxy_user_builder.add_name(proxy_username);
        auto proxy_user = proxy_user_builder.Finish();

        kdd::messageBuilder message_builder{builder};
        message_builder.add_minimum_protocol_version(430);
        message_builder.add_user(user);
        message_builder.add_proxy_user(proxy_user);
        message_builder.add_api_number(kdd::api_no_data_object_open);
        message_builder.add_payload(payload);
        auto msg = message_builder.Finish();

        builder.Finish(msg);

        using int_type = boost::endian::little_int32_buf_t;
        int_type v(builder.GetSize());
        fmt::print("message size (binary): {}\n", v);
        s.write((char*) &v, sizeof(int_type));
        s.write((char*) builder.GetBufferPointer(), builder.GetSize());
    }
    catch (const std::exception& e) {
        fmt::print(stderr, "Exception: {}\n", e.what());
        return 1;
    }

    return 0;
}
