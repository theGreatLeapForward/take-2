#include <dpp/dpp.h>
#include <pqxx/pqxx>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <fmt/format.h>

const std::string token = "MTEzMDM4NDQ4Njk3OTM1NDYyNQ.GbWsRI.0iMrv9zigTekcQYqhd7PExZs2zWvC4FWWCktbw";
// this token dont work lol

auto log_init(dpp::cluster& bot, const std::string& name) {
    /* Set up spdlog logger (stolen) */
    spdlog::init_thread_pool(8192, 2);
    std::vector<spdlog::sink_ptr> sinks;
    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt >();
    auto rotating = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(name, 1024 * 1024 * 5, 10);
    sinks.push_back(stdout_sink);
    sinks.push_back(rotating);
    auto log = std::make_shared<spdlog::async_logger>("logs", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    spdlog::register_logger(log);
    log->set_pattern("%^%Y-%m-%d %H:%M:%S.%e [%L] [th#%t]%$ : %v");
    log->set_level(spdlog::level::level_enum::debug);
    log->flush_on(spdlog::level::info); // does not save to file


    /* Integrate spdlog logger to D++ log events */
    bot.on_log([&log](const dpp::log_t & event) {
        switch (event.severity) {
            case dpp::ll_trace:
                log->trace("{}", event.message);
                break;
            case dpp::ll_debug:
                log->debug("{}", event.message);
                break;
            case dpp::ll_info:
                log->info("{}", event.message);
                break;
            case dpp::ll_warning:
                log->warn("{}", event.message);
                break;
            case dpp::ll_error:
                log->error("{}", event.message);
                break;
            case dpp::ll_critical:
            default:
                log->critical("{}", event.message);
                break;
        }
    });

    return log;
}

int main() {
    using fmt::format;

    auto bot = dpp::cluster(token, dpp::i_default_intents | dpp::i_message_content | dpp::i_guild_members);
    auto log = log_init(bot, "bot.log");

    [[maybe_unused]]
    const auto debug = [&bot](const pqxx::result& data){
        bot.log(dpp::ll_debug, format("Logging of query {}", data.query()));
        for (const auto& row: data) {
            bot.log(dpp::ll_info, format("Row {}", row.num()));
            for (const auto& item: row) {
                bot.log(dpp::ll_info, item.as<std::string>(""));
            }
        }
    };

    pqxx::connection readconn("host=localhost port=5432 dbname=template1 connect_timeout=10 password=goofy user=postgres");

    readconn.prepare("media_get", "SELECT * FROM media WHERE media_name = $1");
    readconn.prepare("channel_active", "SELECT EXISTS (SELECT 1 FROM active WHERE channel_id = $1)");

    {
        auto w = pqxx::work{readconn};
        auto media = w.exec(
                "CREATE TABLE IF NOT EXISTS media ("
                "media_path text, "
                "media_name text"
                ");"
        );
        auto active = w.exec("CREATE TABLE IF NOT EXISTS active (channel_id bigint);");
        w.commit();
    }

    const auto channel_active = [&readconn, &debug](dpp::snowflake channel){
        return pqxx::perform([channel = static_cast<uint64_t>(channel), &readconn, &debug] {
            auto work = pqxx::work{readconn};
            auto data = work.exec_prepared("channel_active", channel);
            work.commit();

            //debug(data);
            return data[0][0].as<std::string>() == "f"; // this is kinda hacky tbh
        });
    };

    dpp::snowflake user;

    bot.on_message_create([&bot, &user, &channel_active](const auto& ctx){
        auto id = ctx.msg.author.id;
        if (channel_active(ctx.msg.channel_id) or id == user) {
            return;
        }

        ctx.reply("keep yourself safe");
        bot.log(dpp::ll_info, format("Responded to {} :D", id));
    });


    bot.on_slashcommand([&bot, &readconn, &channel_active](const auto& ctx){
        ctx.thinking();
        auto cmd = ctx.command.get_command_interaction();
        if (cmd.name == "media") {
            auto subcommand = cmd.options[0];
            if (subcommand.name == "save") {
                auto path = ctx.command.resolved.attachments.find(subcommand.template get_value<dpp::snowflake>(0))->second.url;
                auto name = subcommand.template get_value<std::string>(1);

                auto conn = pqxx::connection{readconn.options()};
                // new connection to ensure thread safety (this probably has holes)

                pqxx::perform([&conn, &path, &name] {
                   auto work = pqxx::work{conn};
                   work.exec_params("INSERT INTO media (media_path, media_name) VALUES ($1, $2)", path, name);
                   work.commit();
                });

                bot.log(dpp::ll_debug, format("Inserted name: {}, path: {}", name, path));

                ctx.edit_original_response(dpp::message(format("Saved media as {}.", name)));
            }
            else if (subcommand.name == "get") {
                auto name = std::get<std::string>(subcommand.options.at(0).value);
                auto opt_path = pqxx::perform([&readconn, &name] {
                    auto work = pqxx::work{readconn};
                    auto got = work.exec_prepared("media_get", name);
                    work.commit();
                    return got[0][0]; // TODO I think this might dangle
                });

                if (!opt_path.is_null()) {
                    auto path = opt_path.template as<std::string>();
                    bot.log(dpp::ll_debug, format("Returned path {} for name {}", path, name));
                    ctx.edit_original_response(dpp::message(format("{}: {}", name, path)));
                } else {
                    bot.log(dpp::ll_warning, format("Returned non-existent path for name {}", name));
                    ctx.edit_original_response(dpp::message(format("Are you stupid or something? That doesn't exist. (Entered name: {})", name)));
                }
            }
        }/*
        else if (cmd.name == "toggle") {
            const auto channel = cmd.template get_value<dpp::snowflake>(0);
            auto conn = pqxx::connection{readconn.options()};

            const auto [func, response] = [&channel_active, channel, &conn] {
                if (channel_active(channel)) {
                    auto f = [&conn] {
                        auto work = pqxx::work{conn};
                    };
                }
                else {
                    return std::make_pair([] {}, "");
                }
            }();

            pqxx::perform(func);
            ctx.edit_original_response(dpp::message(response));
        }*/

    });

    bot.on_ready([&bot, &user](const auto& ctx){
        using sc = dpp::slashcommand;
        using co = dpp::command_option;

        user = bot.current_user_get_sync().id;
        bot.log(dpp::ll_info, format("Bot startup"));
        bot.global_bulk_command_create(std::vector{
            sc{"media", ".", bot.me.id}.
            add_option(co{dpp::co_sub_command, "save", "save media"}.
                add_option(co{dpp::co_attachment, "media", ".", true}).
                add_option(co{dpp::co_string, "name", ".", true})).
            add_option(co{dpp::co_sub_command, "get", "get media"}.
                add_option(co{dpp::co_string, "name", ".", true})),
            sc{"toggle", "toggles whether the bot active (responding to messages) in a channel", bot.me.id}.
                add_option(co{dpp::co_channel, "channel", "the channel to toggle", true})
        });
    });

    bot.start(dpp::st_wait);
    return 0;
}
