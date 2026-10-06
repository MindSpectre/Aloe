#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <fixed_string.hpp>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/core/MacroMetadata.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>

namespace aloe::log {

    /// Severity, lowest first. `None` on a logger silences it.
    enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warning, Error, Critical, None };

    namespace detail {

        [[nodiscard]] constexpr quill::LogLevel to_quill(const LogLevel level) noexcept {
            switch (level) {
                case LogLevel::Trace:
                    return quill::LogLevel::TraceL1;
                case LogLevel::Debug:
                    return quill::LogLevel::Debug;
                case LogLevel::Info:
                    return quill::LogLevel::Info;
                case LogLevel::Warning:
                    return quill::LogLevel::Warning;
                case LogLevel::Error:
                    return quill::LogLevel::Error;
                case LogLevel::Critical:
                    return quill::LogLevel::Critical;
                case LogLevel::None:
                    return quill::LogLevel::None;
            }
            return quill::LogLevel::None;
        }

        /// The sink `logger()` hands to new loggers: set by Logging, the console until then.
        [[nodiscard]] inline std::shared_ptr<quill::Sink>& default_sink() {
            static std::shared_ptr<quill::Sink> sink;
            return sink;
        }

        /// The level `logger()` gives new loggers.
        [[nodiscard]] inline LogLevel& default_level() noexcept {
            static LogLevel level = LogLevel::Info;
            return level;
        }

        [[nodiscard]] inline std::shared_ptr<quill::Sink> console_sink() {
            return quill::Frontend::create_or_get_sink<quill::ConsoleSink>("aloe-console");
        }

    }  // namespace detail

    /**
     * @brief A named logger: a copyable handle, cheap to pass by value.
     *
     * Every call takes its format string as a template parameter, so each call site owns one
     * static metadata record the way quill's macros would, and no macro is needed. Formatting
     * happens on the backend thread; the caller only copies the arguments into a per-thread queue.
     * Arguments are what quill accepts: integers, floating point, `std::string`, `std::string_view`,
     * C strings, and the standard containers quill has codecs for.
     */
    class Logger {
    public:
        explicit Logger(quill::Logger* logger) noexcept
            : logger_{logger} {
        }

        template <LogLevel Level, core::FixedString Format, typename... Args>
        void log(Args&&... args) const {
            static constexpr quill::MacroMetadata metadata{
                "", "", Format.data(), nullptr, detail::to_quill(Level), quill::MacroMetadata::Event::Log};
            if (logger_->template should_log_statement<detail::to_quill(Level)>()) {
                logger_->template log_statement<false>(&metadata, std::forward<Args>(args)...);
            }
        }

        template <core::FixedString Format, typename... Args>
        void trace(Args&&... args) const {
            log<LogLevel::Trace, Format>(std::forward<Args>(args)...);
        }

        template <core::FixedString Format, typename... Args>
        void debug(Args&&... args) const {
            log<LogLevel::Debug, Format>(std::forward<Args>(args)...);
        }

        template <core::FixedString Format, typename... Args>
        void info(Args&&... args) const {
            log<LogLevel::Info, Format>(std::forward<Args>(args)...);
        }

        template <core::FixedString Format, typename... Args>
        void warning(Args&&... args) const {
            log<LogLevel::Warning, Format>(std::forward<Args>(args)...);
        }

        template <core::FixedString Format, typename... Args>
        void error(Args&&... args) const {
            log<LogLevel::Error, Format>(std::forward<Args>(args)...);
        }

        template <core::FixedString Format, typename... Args>
        void critical(Args&&... args) const {
            log<LogLevel::Critical, Format>(std::forward<Args>(args)...);
        }

        void set_level(const LogLevel level) const noexcept {
            logger_->set_log_level(detail::to_quill(level));
        }

        /// Blocks until the backend has written everything this logger queued. Cold path only, and only while a
        /// `Logging` is alive: quill spins until the backend acknowledges, so with no backend this never returns.
        void flush() const {
            logger_->flush_log();
        }

    private:
        quill::Logger* logger_;
    };

    struct LoggingConfig {
        LogLevel level = LogLevel::Info;
        std::optional<std::uint16_t> backend_cpu =
            std::nullopt;  ///< Pin the backend thread here; never a shard's core.
        std::optional<std::filesystem::path> file = std::nullopt;  ///< Write here, truncating, instead of the console.
    };

    /**
     * @brief Owns the logging backend for the process. Construct one, once, before the first log line.
     *
     * Starts quill's backend thread with the sink and level the config asks for; the destructor
     * flushes and stops it. Loggers created before it exists write to the console at Info. Tests
     * construct one through aloe::testing::LoggingEnvironment.
     */
    class Logging {
    public:
        explicit Logging(const LoggingConfig& config = {}) {
            quill::BackendOptions options;
            if (config.backend_cpu) {
                options.cpu_affinity = {*config.backend_cpu};
            }
            if (!quill::Backend::is_running()) {
                quill::Backend::start(options);
            }
            if (config.file) {
                quill::FileSinkConfig file_config;
                file_config.set_open_mode('w');
                detail::default_sink() = quill::Frontend::create_or_get_sink<quill::FileSink>(
                    config.file->string(), file_config, quill::FileEventNotifier{});
            } else {
                detail::default_sink() = detail::console_sink();
            }
            detail::default_level() = config.level;
        }

        Logging(const Logging&)            = delete;
        Logging& operator=(const Logging&) = delete;
        Logging(Logging&&)                 = delete;
        Logging& operator=(Logging&&)      = delete;

        ~Logging() {
            quill::Backend::stop();
            detail::default_sink().reset();
            detail::default_level() = LogLevel::Info;
        }
    };

    /**
     * @brief The logger called `name`, created on first use with the process's default sink and level.
     *
     * quill keeps loggers by name: a second call with the same name returns the first logger, with
     * the sink it was created with. Module code names its logger after the module, `"aloe.runtime"`.
     */
    [[nodiscard]] inline Logger logger(const std::string& name) {
        std::shared_ptr<quill::Sink> sink = detail::default_sink();
        if (!sink) {
            sink = detail::console_sink();
        }
        Logger result{quill::Frontend::create_or_get_logger(name, std::move(sink))};
        result.set_level(detail::default_level());
        return result;
    }

}  // namespace aloe::log
