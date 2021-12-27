#include "message_generated.h"

#include <flatbuffers/flatbuffer_builder.h>
#include <fmt/format.h>

#include <iostream>

namespace fbs = flatbuffers;

int main(int _argc, char** _argv)
{
    if (_argc < 2) {
        fmt::print("Missing argument: <payload>\n");
        fmt::print("USAGE: test_fbs_message <payload>\n");
        return 1;
    }

    fbs::FlatBufferBuilder builder{1024};

    auto username = builder.CreateString("kory");
    auto proxy_username = builder.CreateString("rods");
    auto payload = builder.CreateString(_argv[1]);

    kdd::scpps::user_infoBuilder user_builder{builder};
    user_builder.add_name(username);
    auto user = user_builder.Finish();

    kdd::scpps::user_infoBuilder proxy_user_builder{builder};
    proxy_user_builder.add_name(proxy_username);
    auto proxy_user = proxy_user_builder.Finish();

    kdd::scpps::messageBuilder message_builder{builder};
    message_builder.add_minimum_protocol_version(430);
    message_builder.add_user(user);
    message_builder.add_proxy_user(proxy_user);
    message_builder.add_api_number(kdd::scpps::api_no_data_object_open);
    message_builder.add_payload(payload);
    auto msg = message_builder.Finish();

    builder.Finish(msg);

    for (fbs::uoffset_t i = 0, j = 0; i < builder.GetSize(); ++i) {
        fmt::print("{:02x} ", builder.GetBufferPointer()[i]);

        if (++j == 16) {
            j = 0;
            fmt::print("\n");
        }
    }

    fmt::print("\n");

    return 0;
}

