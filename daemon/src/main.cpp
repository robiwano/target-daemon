// Target deamon
//
// main.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2016 Robert Bielik (robert dot bielik at dirac dot com)
//
//

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <chrono>

#include <asio.hpp>
#include <asio/deadline_timer.hpp>

using asio::ip::tcp;
using namespace std;

#if RASPBERRY_PI
#include <pigpio.h>
#endif

namespace
{
    atomic<int> sessionCntr{};
    atomic<int> noOfSessions{};

    string audioPlayCmdLinePrefix_{};
    int sessionTimeout_ = 10;   // Inactivity timeout before session terminates.

    // trim from start
    static string ltrim(string&& s) {
        s.erase(s.begin(), find_if(s.begin(), s.end(),
            not1(ptr_fun<int, int>(isspace))));
        return move(s);
    }

    // trim from end
    static string rtrim(string&& s) {
        s.erase(find_if(s.rbegin(), s.rend(),
            not1(ptr_fun<int, int>(isspace))).base(), s.end());
        return move(s);
    }

    // trim from both ends
    static string trim(string&& s) {
        return ltrim(rtrim(move(s)));
    }

    static vector<string> split_commands(string s, string delimiters = ";\n")
    {
        vector<string> retval;
        auto append = [&](string s)
        {
            auto entry = trim(move(s));
            if (!entry.empty())
                retval.emplace_back(move(entry));
        };
        size_t last = 0;
        size_t next = 0;
        while ((next = s.find_first_of(delimiters, last)) != string::npos)
        {
            append(s.substr(last, next - last));
            last = next + 1;
        }
        append(s.substr(last));
        return move(retval);
    }


    struct session : enable_shared_from_this<session>
    {
        typedef chrono::high_resolution_clock clock_type;

        tcp::socket socket_;
        asio::steady_timer timer_;
        const int sessionNumber_;
        const bool activeSession_;
        enum { max_length = 1024 };
        array<char, max_length> data_;

        struct step
        {
            chrono::seconds time_to_execute_;
            function<void()> fn_;
            step(chrono::seconds t, function<void()> fn) : time_to_execute_(t), fn_(fn) {}
        };

        vector<step> program_;
        mutex mutex_;
        bool stop_flag_{};
        condition_variable cv_;
        future<void> programJob_;
        clock_type::time_point programStartTime_;

#if RASPBERRY_PI
        // Raspberry Pi implementation
        struct target_control
        {
            atomic<bool> position_{};

            enum GPIO
            {
                ENABLE = 0,
                TURN_FRONT = 1,
                TURN_AWAY = 4,
            };

            target_control()
            {
                if (gpioInitialise() < 0)
                    throw runtime_error("GPIO not available!!");

                gpioSetMode(GPIO::TURN_FRONT, PI_OUTPUT);
                gpioSetMode(GPIO::TURN_AWAY, PI_OUTPUT);
                gpioSetMode(GPIO::ENABLE, PI_OUTPUT);

                gpioWrite(GPIO::TURN_FRONT, 0);
                gpioWrite(GPIO::TURN_AWAY, 0);
                gpioWrite(GPIO::ENABLE, 1);
            }
            ~target_control()
            {
                gpioWrite(GPIO::TURN_FRONT, 0);
                gpioWrite(GPIO::TURN_AWAY, 0);
                gpioWrite(GPIO::ENABLE, 0);

                gpioTerminate();
            }

            future<void> move_target(bool toFront)
            {
                return async(launch::async, [this, toFront]() mutable
                {
                    position_ = toFront ? 1 : 0;
                    const int gpioBit = toFront ? GPIO::TURN_FRONT : GPIO::TURN_AWAY;
                    gpioWrite(gpioBit, 1);
                    this_thread::sleep_for(chrono::milliseconds(500));
                    gpioWrite(gpioBit, 0);
                });
            }

            bool position() const
            {
                return position_;
            }
        };
#else
        // Dummy target implementation
        struct target_control
        {
            atomic<bool> position_{};

            future<void> move_target(bool toFront)
            {
                return async(launch::async, [this, toFront]() mutable { position_ = toFront; });
            }

            bool position() const
            {
                return position_;
            }
        };
#endif
        unique_ptr<target_control> target_control_{};

        session(tcp::socket socket)
            : socket_(move(socket))
            , timer_(socket_.get_io_context())
            , sessionNumber_(++sessionCntr)
            , activeSession_(noOfSessions == 1)
        {
            cout << "Session " << sessionNumber_ << " started" << (activeSession_ ? " (active)." : ".") << endl;
            if (activeSession_)
            {
                try
                {
                    target_control_.reset(new target_control());
                }
                catch (exception& e)
                {
                    cerr << "Exception: " << e.what() << endl;
                }
            }
        }
        ~session()
        {
            stop_program();
            cout << "Session " << sessionNumber_ << " stopped." << endl;
            --noOfSessions;
        }

        void start()
        {
            do_read();
        }

        void set_timer()
        {
            timer_.async_wait([this](error_code ec)
            {
                if (ec != asio::error::operation_aborted)
                {
                    socket_.shutdown(asio::socket_base::shutdown_both);
                }
                else
                    set_timer();
            });
        }

        bool is_executing()
        {
            if (programJob_.valid())
            {
                if (programJob_.wait_for(chrono::seconds(0)) == future_status::timeout)
                    return true;
                programJob_.get();
            }
            return false;
        }

        void stop_program()
        {
            if (is_executing())
            {
                stop_flag_ = true;
                cv_.notify_one();
                programJob_ = future<void>{};
            }
        }

        void start_program()
        {
            if (is_executing())
                throw runtime_error("Executing");

            if (program_.empty())
                throw runtime_error("Empty");

            cout << "Started program with " << program_.size() << " steps..." << endl;

            stop_flag_ = false;

            programJob_ = async(launch::async, [this]
            {
                programStartTime_ = clock_type::now();
                auto currentStepTime = programStartTime_;
                for (auto& current_step : program_)
                {
                    currentStepTime += current_step.time_to_execute_;
                    {
                        unique_lock<mutex> lock(mutex_);
                        if (cv_.wait_until(lock, currentStepTime, [&] { return stop_flag_; }))
                        {
                            cout << "Program stopped!" << endl;
                            return;
                        }
                    }
                    if (current_step.fn_)
                    {
                        auto t_relative = chrono::duration_cast<chrono::seconds>(currentStepTime - programStartTime_).count();
                        cout << "T" << t_relative << ": ";
                        try
                        {
                            current_step.fn_();
                        }
                        catch (exception& e)
                        {
                            cout << " Error: " << e.what();
                        }
                        cout << endl;
                    }
                }
                cout << "Program ended!" << endl;
            }
            );
        }

        string parse_command(const string& s)
        {
            try
            {
                switch (s.front())
                {
                case 'C':   // Clear program
                    stop_program();
                    program_.clear();
                    cout << "Program cleared!" << endl;
                    break;

                case 'T':
                    program_.emplace_back(chrono::seconds(stoi(s.substr(1))), function<void()>{});
                    break;

                case 'A':   // Play audio
                {
                    auto arg = s.substr(1);
                    if (arg.empty())
                        throw runtime_error("Syntax");

                    program_.emplace_back(chrono::seconds::zero(), [this, arg]
                    {
                        cout << "Playing audio file '" << arg << "';";
                        if (!audioPlayCmdLinePrefix_.empty())
                        {
                            string cmdline = audioPlayCmdLinePrefix_;
                            cmdline += " ";
                            cmdline += arg;
                            thread([cmdline] { system(cmdline.c_str()); }).detach();
                        }

                    });
                }
                break;

                case 'M':   // Move target
                {
                    auto arg = stoi(s.substr(1));
                    program_.emplace_back(chrono::seconds::zero(), [this, arg]
                    {
                        cout << "Moving target to position '" << arg << "';";
                        if (target_control_)
                            target_control_->move_target(!!arg);
                    });
                }
                break;

                case 'P':   // Play audio file directly
                {
                    if (is_executing())
                        throw runtime_error("Executing");

                    auto arg = s.substr(1);
                    if (arg.empty())
                        throw runtime_error("Syntax");

                    cout << "Playing audio file '" << arg << "' directly\n";
                    if (!audioPlayCmdLinePrefix_.empty())
                    {
                        string cmdline = audioPlayCmdLinePrefix_;
                        cmdline += " ";
                        cmdline += arg;
                        thread([cmdline] { system(cmdline.c_str()); }).detach();
                    }
                    break;
                }

                case 'D':   // Move target directly
                    if (is_executing())
                        throw runtime_error("Executing");

                    if (target_control_)
                    {
                        auto arg = stoi(s.substr(1));
                        cout << "Moving target to position '" << arg << "'" << endl;
                        target_control_->move_target(!!arg);
                    }
                    else
                        throw runtime_error("Target");
                    break;

                case 'R':   // Run program
                    start_program();
                    break;

                case 'S':   // Stop program
                    stop_program();
                    break;

                case 'Q':   // Query state
                {
                    stringstream msg;
                    auto t_relative = chrono::duration_cast<chrono::seconds>(clock_type::now() - programStartTime_).count();
                    auto t_total = chrono::seconds::zero();
                    for (auto& step : program_)
                        t_total += step.time_to_execute_;

                    msg << "EXEC=" << (is_executing() ? to_string(t_relative) : "") << "\r\n"
                        << "PROG=" << (!program_.empty() ? to_string(t_total.count()) : "") << "\r\n"
                        << "POS=" << (target_control_ ? to_string(int(target_control_->position())) : "") << "\r\n";
                    return msg.str();
                }
                break;

                case 'X':
                    cout << "Stopping server..." << endl;
                    socket_.get_io_context().stop();
                    break;

                default:
                    throw runtime_error("UnknownCommand");
                }

                return move(string{});
            }
            catch (runtime_error& e)
            {
                throw e;
            }
            catch (exception&)
            {
                throw runtime_error("Syntax");
            }
        }

        void do_read()
        {
            if (sessionTimeout_ > 0)
            {
                // Start watchdog timer
                timer_.cancel();
                timer_.expires_after(chrono::seconds(sessionTimeout_));
                set_timer();
            }

            auto self = shared_from_this();
            socket_.async_read_some(asio::buffer(data_, max_length),
                [this, self](error_code ec, size_t length)
            {
                if (!ec)
                {
                    if (!activeSession_)
                    {
                        do_write("ERROR=Busy\r\n");
                    }
                    else
                    {
                        try
                        {
                            auto cmds = split_commands(move(string{ data_.data(), length }));
                            if (!cmds.empty())
                            {
                                string reply;
                                for (auto& cmd : cmds)
                                    reply += parse_command(cmd);

                                if (!reply.empty())
                                    do_write(reply);
                                else
                                    do_write("OK\r\n");
                            }
                            else
                                do_read();
                        }
                        catch (const exception& e)
                        {
                            stringstream msg;
                            msg << "ERROR=" << e.what() << "\r\n";
                            do_write(msg.str());
                        }
                    }
                }
            });
        }

        void do_write(const string& msg)
        {
            size_t len = msg.copy(data_.data(), max_length);
            auto self = shared_from_this();
            asio::async_write(socket_, asio::buffer(data_, len),
                [this, self](error_code ec, size_t /*length*/)
            {
                if (!ec)
                {
                    do_read();
                }
            });
        }
    };

    struct server
    {
        tcp::acceptor acceptor_;

        server(asio::io_context& io_context, short port)
            : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
        {
            do_accept();
        }

        void do_accept()
        {
            acceptor_.async_accept(
                [this](error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    ++noOfSessions;
                    make_shared<session>(move(socket))->start();
                }

                do_accept();
            });
        }
    };

    struct input_parser
    {
        vector<string> tokens_;

        input_parser(int argc, char **argv) {
            for (int i = 1; i < argc; ++i)
                tokens_.push_back(string(argv[i]));
        }

        string getCmdOption(const string &option) const {
            vector<string>::const_iterator itr;
            itr = find(tokens_.begin(), tokens_.end(), option);
            if (itr != tokens_.end() && ++itr != tokens_.end()) {
                return *itr;
            }
            return string{};
        }

        bool cmdOptionExists(const string &option) const {
            return find(tokens_.begin(), tokens_.end(), option)
                != tokens_.end();
        }
    };
}

void usage()
{
    cerr << "Usage: ./target_daemon [--port <port (default 5000)>] [--play-cmd <audio play cmd line prefix>] [--timeout <watchdog timeout in seconds (default 20), zero disables watchdog]\n";
}

int main(int argc, char* argv[])
{
    try
    {
        input_parser opts(argc, argv);

        int port = 5000;
        auto port_opt = opts.getCmdOption("--port");
        if (!port_opt.empty())
            port = stoi(port_opt);

        audioPlayCmdLinePrefix_ = opts.getCmdOption("--play-cmd");
        if (!audioPlayCmdLinePrefix_.empty())
            cout << "Audio play prefix: '" << audioPlayCmdLinePrefix_ << "'\n";

        auto timeout_opt = opts.getCmdOption("--timeout");
        if (!timeout_opt.empty())
        {
            sessionTimeout_ = stoi(timeout_opt);
            if (sessionTimeout_ == 0)
                cout << "Warning: Session timeout disabled!\n";
        }

        asio::io_context io_context;
        server s(io_context, port);
        cout << "Daemon started listening on port " << port << endl;
        io_context.run();
    }
    catch (exception& e)
    {
        cerr << "Exception: " << e.what() << "\n";
        usage();
        return 1;
    }
    return 0;
}
