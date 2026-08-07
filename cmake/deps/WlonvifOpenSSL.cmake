include_guard(GLOBAL)
# Core uses OpenSSL crypto; the web example requests SSL separately.
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
