include_guard(GLOBAL)

# Beast/Asio are header-only here; JSON and TLS provide linked components.
find_package(Boost 1.89 CONFIG REQUIRED COMPONENTS json)
find_package(OpenSSL 3 REQUIRED COMPONENTS SSL Crypto)
