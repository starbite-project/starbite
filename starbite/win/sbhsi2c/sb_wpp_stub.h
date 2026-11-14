/* Starbite build of the Exynos9810 HSI2C driver without the WPP preprocessor (tools/win/build.sh has no tracewpp):
 * the trace calls compile to nothing. Included by the three *.tmh stand-ins. */
#pragma once
#define WPP_INIT_TRACING(DriverObject, RegistryPath) ((void)0)
#define WPP_CLEANUP(DriverObject) ((void)0)
#define Trace(...) ((void)0)
#define FuncEntry(...) ((void)0)
#define FuncExit(...) ((void)0)
