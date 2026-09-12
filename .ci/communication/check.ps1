$ErrorActionPreference = "Stop"
. (Join-Path $env:CONDA_PREFIX "Library/dev-env.ps1")
$repository = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$staging = Join-Path $PSScriptRoot "install"
$httpDependencies = Join-Path $PSScriptRoot "vcpkg/installed/x64-windows"
$env:CMAKE_PREFIX_PATH = "$staging;$env:CMAKE_PREFIX_PATH;$httpDependencies"
$env:PKG_CONFIG_PATH = "$staging/lib/pkgconfig;$env:PKG_CONFIG_PATH"
$env:PATH = "$staging/bin;$staging/lib;$staging/lib/orocos/win32/types;$staging/lib/orocos/win32/plugins;$staging/lib/orocos/win32/ocl/types;$httpDependencies/bin;$env:PATH"
$env:RTT_COMPONENT_PATH = "$staging/lib/orocos"
$env:OROCOS_COMPONENT_PATH = $env:RTT_COMPONENT_PATH

& cmake -S "$PSScriptRoot/rtt" -B "$PSScriptRoot/build-rtt" -G Ninja `
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$staging" `
    "-DDEFAULT_PLUGIN_PATH=$staging/lib/orocos" `
    -DOROCOS_TARGET=win32 -DENABLE_CORBA=OFF -DENABLE_TESTS=OFF `
    -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DOROBLD_FORCE_TINY_DEMARSHALLER=ON `
    -DORO_OS_USE_BOOST_THREAD=ON -DPLUGINS_ENABLE=ON -DPLUGINS_ENABLE_TYPEKIT=ON `
    -DPLUGINS_ENABLE_SCRIPTING=ON -DPLUGINS_ENABLE_MARSHALLING=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build "$PSScriptRoot/build-rtt" --parallel 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --install "$PSScriptRoot/build-rtt"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake -S "$PSScriptRoot/rtt_opcua" -B "$PSScriptRoot/build-opcua" -G Ninja `
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$staging" -DBUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build "$PSScriptRoot/build-opcua" --parallel 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --install "$PSScriptRoot/build-opcua"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake -S "$PSScriptRoot/rtt_http" -B "$PSScriptRoot/build-http" -G Ninja `
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$staging" `
    -DBUILD_TESTING=OFF "-DRTT_HTTP_HTTPLIB_INCLUDE_DIR=$PSScriptRoot/cpp-httplib" `
    "-DOPENSSL_ROOT_DIR=$httpDependencies"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build "$PSScriptRoot/build-http" --parallel 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --install "$PSScriptRoot/build-http"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake -S $repository -B "$PSScriptRoot/build-ocl" -G Ninja `
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$staging" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DBUILD_TESTING=ON -DBUILD_TESTS=OFF -DBUILD_HTTP=ON -DBUILD_OPCUA=ON `
    -DBUILD_LOGGING=OFF -DBUILD_LUA_RTT=OFF -DBUILD_REPORTING_NETCDF=OFF -DBUILD_DOCS=OFF `
    "-DOCL_HTTP_TEST_HTTPLIB_INCLUDE_DIR=$PSScriptRoot/cpp-httplib"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build "$PSScriptRoot/build-ocl" --parallel 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --install "$PSScriptRoot/build-ocl"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ctest --test-dir "$PSScriptRoot/build-ocl" -R '^ocl_http_deployment$' --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ctest --test-dir "$PSScriptRoot/build-ocl" -R '^ocl_opcua_deployment_' --output-on-failure --no-tests=error
exit $LASTEXITCODE
