#include "example_support.hpp"

#include <iostream>

#define REQUIRE(expression)                                                                        \
    do                                                                                             \
    {                                                                                              \
        if (!(expression))                                                                         \
        {                                                                                          \
            std::cerr << "requirement failed: " #expression " at line " << __LINE__ << '\n';       \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main()
{
    using namespace wlonvif::examples;
    std::size_t size = 0;
    REQUIRE(parse_size("16", size));
    REQUIRE(size == 16);
    REQUIRE(!parse_size("16x", size));
    REQUIRE(json_escape("a\n\"b") == "a\\n\\\"b");
    REQUIRE(redact_url("http://example-user:example-password@camera/path") ==
            "http://[redacted]@camera/path");
    REQUIRE(redact_url("http://camera/path") == "http://camera/path");

    char executable[] = "example";
    char timeout[] = "--timeout";
    char seconds[] = "12";
    char json[] = "--json";
    char password_option[] = "--password";
    char password[] = "example-command-line-password";
    char custom[] = "--custom";
    char* arguments[]{executable, timeout, seconds, json, password_option, password, custom};
    auto parsed = parse_common_arguments(7, arguments);
    REQUIRE(parsed);
    REQUIRE(parsed.value().common.timeout_seconds == 12);
    REQUIRE(parsed.value().common.timeout_set);
    REQUIRE(parsed.value().common.password == password);
    REQUIRE(wlonvif::examples::shell_quote("a'b") == "'a'\\''b'");
    REQUIRE(wlonvif::examples::redact_url(
                "rtsp://example-user:example-password@camera/live"
            ) ==
            "rtsp://[redacted]@camera/live");
    const std::optional<wlonvif::Credentials> url_credentials(
        wlonvif::Credentials{"example user", "example@password:with-punctuation", true}
    );
    REQUIRE(url_with_credentials("rtsp://camera/live", url_credentials) ==
            "rtsp://example%20user:example%40password%3Awith-punctuation@camera/live");
    REQUIRE(url_with_credentials(
                "rtsp://old-example-user:old-example-password@camera/live", url_credentials
            ) ==
            "rtsp://example%20user:example%40password%3Awith-punctuation@camera/live");
    REQUIRE(url_with_credentials("not-a-url", url_credentials) == "not-a-url");
    REQUIRE(parsed.value().common.json);
    REQUIRE(parsed.value().remaining == std::vector<std::string>{"--custom"});

    CommonOptions direct;
    direct.username = "example-user";
    direct.password = "example-password";
    auto direct_credentials = load_credentials(direct);
    REQUIRE(direct_credentials);
    REQUIRE(direct_credentials.value());
    REQUIRE(direct_credentials.value()->username == "example-user");
    REQUIRE(direct_credentials.value()->password == "example-password");

    auto runtime = wlonvif::Runtime::create();
    REQUIRE(runtime);
    auto explicit_endpoint = resolve_device_endpoint(
        "http://camera:2020/onvif/device_service", {}, runtime.value(), 1
    );
    REQUIRE(explicit_endpoint);
    REQUIRE(explicit_endpoint.value().url == "http://camera:2020/onvif/device_service");
    REQUIRE(!resolve_device_endpoint({}, {}, runtime.value(), 1));
    REQUIRE(!resolve_device_endpoint("http://camera/device", "192.0.2.1", runtime.value(), 1));
    REQUIRE(runtime.value()->shutdown());

    CommonOptions anonymous;
    anonymous.password_environment = "WLONVIF_TEST_PASSWORD_DOES_NOT_EXIST";
    auto credentials = load_credentials(anonymous);
    REQUIRE(credentials);
    REQUIRE(!credentials.value());
    std::cout << "example support tests passed\n";
}
