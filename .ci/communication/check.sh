#!/usr/bin/env bash
set -euo pipefail
source "$CONDA_PREFIX/dev-env.sh"
ci_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository="$(cd -- "$ci_directory/../.." && pwd)"
staging="$ci_directory/install"
export CMAKE_PREFIX_PATH="$staging:${CMAKE_PREFIX_PATH:-}"
export PKG_CONFIG_PATH="$staging/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$staging/lib:$staging/lib/orocos/gnulinux/types:$staging/lib/orocos/gnulinux/plugins:$staging/lib/orocos/gnulinux/ocl/types:$staging/lib/orocos/gnulinux/rtt_opcua/plugins:${LD_LIBRARY_PATH:-}"
export RTT_COMPONENT_PATH="$staging/lib/orocos"
export OROCOS_COMPONENT_PATH="$RTT_COMPONENT_PATH"
# The released SDK may require a newer compatible libstdc++ runtime than the
# compiler's private copy. Link to the runtime selected in this environment.
export LDFLAGS="-L$CONDA_PREFIX/lib ${LDFLAGS:-}"

cmake -S "$ci_directory/rtt" -B "$ci_directory/build-rtt" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$staging" \
  -DDEFAULT_PLUGIN_PATH="$staging/lib/orocos" \
  -DENABLE_CORBA=OFF -DENABLE_TESTS=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF \
  -DPLUGINS_ENABLE=ON -DPLUGINS_ENABLE_TYPEKIT=ON \
  -DPLUGINS_ENABLE_SCRIPTING=ON -DPLUGINS_ENABLE_MARSHALLING=ON
cmake --build "$ci_directory/build-rtt" --parallel 2
cmake --install "$ci_directory/build-rtt"
cmake -S "$ci_directory/rtt_opcua" -B "$ci_directory/build-opcua" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$staging" -DBUILD_TESTING=OFF
cmake --build "$ci_directory/build-opcua" --parallel 2
cmake --install "$ci_directory/build-opcua"
cmake -S "$ci_directory/rtt_http" -B "$ci_directory/build-http" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$staging" \
  -DBUILD_TESTING=OFF -DRTT_HTTP_HTTPLIB_INCLUDE_DIR="$ci_directory/cpp-httplib"
cmake --build "$ci_directory/build-http" --parallel 2
cmake --install "$ci_directory/build-http"
cmake -S "$repository" -B "$ci_directory/build-ocl" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$staging" \
  -DCMAKE_SKIP_RPATH=ON -DBUILD_TESTING=ON -DBUILD_TESTS=OFF \
  -DBUILD_HTTP=ON -DBUILD_OPCUA=ON -DBUILD_LOGGING=OFF \
  -DBUILD_LUA_RTT=OFF -DBUILD_REPORTING_NETCDF=OFF -DBUILD_DOCS=OFF \
  -DOCL_HTTP_TEST_HTTPLIB_INCLUDE_DIR="$ci_directory/cpp-httplib"
cmake --build "$ci_directory/build-ocl" --parallel 2
cmake --build "$ci_directory/build-ocl" --parallel 2 --target taskbrowser_value_renderer_test
cmake --install "$ci_directory/build-ocl"
# Each protocol must register tests; optional dependency discovery cannot
# silently turn this combined deployment gate into an OPC-UA-only build.
ctest --test-dir "$ci_directory/build-ocl" -R '^taskbrowser_value_renderer_test$' \
  --output-on-failure --no-tests=error
ctest --test-dir "$ci_directory/build-ocl" -R '^ocl_http_deployment$' \
  --output-on-failure --no-tests=error
ctest --test-dir "$ci_directory/build-ocl" -R '^ocl_opcua_deployment_' \
  --output-on-failure --no-tests=error
