#ifndef __CPYTHON_RUNTIME_H__
#define __CPYTHON_RUNTIME_H__
#include <filesystem>
#include <mutex>
#include <string>
struct _ts;
namespace provider
{
	class CPythonRuntime final
	{
		public:
			CPythonRuntime() = default;
			~CPythonRuntime();
			bool Initialize(const std::filesystem::path& runtimeHome, const std::filesystem::path& scriptPath);
			void Finalize();
			bool IsInitialized() const;
			const std::string& GetLastError() const;

		private:
			mutable std::mutex m_mtxRuntime;
			_ts* m_pMainThreadState{nullptr};
			bool m_bInitialized{false};
			std::string m_strLastError;
	};
} // namespace provider
#endif
