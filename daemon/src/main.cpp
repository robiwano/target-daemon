// Target deamon
//
// main.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2016 Robert Bielik (robert dot bielik at dirac dot com)
//
//

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

#include <asio.hpp>
#include <asio/deadline_timer.hpp>

#include "utility.h"

#include <CLI/CLI.hpp>

using asio::ip::tcp;
using asio::ip::udp;
using namespace std;

#if RASPBERRY_PI
#include <pigpio.h>

enum GPIO {              // GPIO pin
    ENABLE         = 0,  // 11
    TURN_FRONT     = 2,  // 13
    TURN_AWAY      = 3,  // 15
    SERVER_READY   = 1,  // 12
    SESSION_ACTIVE = 4,  // 16
    PROGRAM_ACTIVE = 5,  // 18
    BUTTON         = 6,  // 22 (input active low)
};

struct gpio_init_handler {
    gpio_init_handler()
    {
        if (++instances_ == 1) {
            if (gpioInitialise() < 0)
                throw runtime_error("GPIO not available!!");
            gpioSetMode(GPIO::TURN_FRONT, PI_OUTPUT);
            gpioSetMode(GPIO::TURN_AWAY, PI_OUTPUT);
            gpioSetMode(GPIO::ENABLE, PI_OUTPUT);
            gpioSetMode(GPIO::SERVER_READY, PI_OUTPUT);
            gpioSetMode(GPIO::SESSION_ACTIVE, PI_OUTPUT);
            gpioSetMode(GPIO::PROGRAM_ACTIVE, PI_OUTPUT);
            gpioSetMode(GPIO::BUTTON, PI_INPUT);
            // 20 ms glitch filter on button
            gpioGlitchFilter(GPIO::BUTTON, 20000);
        }
    }
    ~gpio_init_handler()
    {
        if (--instances_ == 0) {
            gpioTerminate();
        }
    }

private:
    static atomic<int> instances_;
};

atomic<int> gpio_init_handler::instances_{0};
#else
struct gpio_init_handler {
};
#endif

namespace
{
    string audioPlayCmdLinePrefix_{};
    int sessionTimeout_ = 20;  // Inactivity timeout before session terminates.

    // trim from start
    static string ltrim(string&& s)
    {
        s.erase(s.begin(), find_if(s.begin(), s.end(), not1(ptr_fun<int, int>(isspace))));
        return move(s);
    }

    // trim from end
    static string rtrim(string&& s)
    {
        s.erase(find_if(s.rbegin(), s.rend(), not1(ptr_fun<int, int>(isspace))).base(), s.end());
        return move(s);
    }

    // trim from both ends
    static string trim(string&& s) { return ltrim(rtrim(move(s))); }

    static vector<string> split_commands(string s, string delimiters = ";\n")
    {
        vector<string> retval;
        auto append = [&](string s) {
            auto entry = trim(move(s));
            if (!entry.empty())
                retval.emplace_back(move(entry));
        };
        size_t last = 0;
        size_t next = 0;
        while ((next = s.find_first_of(delimiters, last)) != string::npos) {
            append(s.substr(last, next - last));
            last = next + 1;
        }
        append(s.substr(last));
        return move(retval);
    }

    struct client_exit {
    };

#if RASPBERRY_PI
    struct server_ready_marker : gpio_init_handler {
        server_ready_marker() { gpioWrite(GPIO::SERVER_READY, 1); }
        ~server_ready_marker() { gpioWrite(GPIO::SERVER_READY, 0); }
    };

    struct session_active_marker : gpio_init_handler {
        server_ready_marker() { gpioWrite(GPIO::SESSION_ACTIVE, 1); }
        ~server_ready_marker() { gpioWrite(GPIO::SESSION_ACTIVE, 0); }
    };

    struct running_program_marker : gpio_init_handler {
        running_program_marker() { gpioWrite(GPIO::PROGRAM_ACTIVE, 1); }
        ~running_program_marker() { gpioWrite(GPIO::PROGRAM_ACTIVE, 0); }
    };

    struct target_control : gpio_init_handler {
        atomic<bool> position_{false};

        target_control()
        {
            gpioWrite(GPIO::TURN_FRONT, 0);
            gpioWrite(GPIO::TURN_AWAY, 0);
            gpioWrite(GPIO::ENABLE, 1);
        }
        ~target_control()
        {
            gpioWrite(GPIO::TURN_FRONT, 0);
            gpioWrite(GPIO::TURN_AWAY, 0);
            gpioWrite(GPIO::ENABLE, 0);
        }

        future<void> move_target(bool toFront)
        {
            return async(launch::async, [this, toFront]() mutable {
                position_         = toFront ? 1 : 0;
                const int gpioBit = toFront ? GPIO::TURN_FRONT : GPIO::TURN_AWAY;
                gpioWrite(gpioBit, 1);
                this_thread::sleep_for(chrono::milliseconds(500));
                gpioWrite(gpioBit, 0);
                this_thread::sleep_for(chrono::milliseconds(50));
            });
        }

        bool position() const { return position_; }
    };

    // Used to manually turn targets back&forth
    struct button_handler : gpio_init_handler {
        unique_ptr<target_control> target_control_{};
        future<void> button_job_;

        static void eventFuncEx(int event, int level, uint32_t tick, void* userdata)
        {
            if (event != GPIO::BUTTON)
                return;

            if (level != 0)  // If not change to low
                return;

            ((button_handler*)userdata)->on_button();
        }

        button_handler()
        {
            try {
                target_control_.reset(new target_control());
                gpioSetAlertFuncEx(GPIO::BUTTON, eventFuncEx, this);
            } catch (exception& e) {
                cerr << "Exception: " << e.what() << endl;
            }
        }
        ~button_handler() { gpioSetAlertFuncEx(GPIO::BUTTON, NULL, NULL); }

        void on_button() { button_job_ = target_control_->move_target(!target_control_->position()); }
    };

#else
    // Dummy implementations
    struct server_ready_marker {
    };
    struct session_active_marker {
    };
    struct running_program_marker {
    };
    struct target_control {
        atomic<bool> position_{};

        future<void> move_target(bool toFront)
        {
            return async(launch::async, [this, toFront]() mutable {
                position_ = toFront;
                this_thread::sleep_for(chrono::milliseconds(500));
            });
        }

        bool position() const { return position_; }
    };

    struct button_handler {
    };
#endif

    struct session : enable_shared_from_this<session> {
        using clock_type = chrono::high_resolution_clock;

        tcp::socket socket_;
        asio::steady_timer timer_;
        enum { max_length = 1024 };
        array<char, max_length> data_;

        struct step {
            chrono::milliseconds time_to_execute_;
            function<void()> fn_;
            step(chrono::milliseconds t, function<void()> fn) : time_to_execute_(t), fn_(fn) {}
        };

        vector<step> program_;
        mutex mutex_;
        bool stop_flag_{};
        condition_variable cv_;
        future<void> programJob_;
        clock_type::time_point programStartTime_;

        typedef function<void()> on_exit_type;
        on_exit_type on_exit_;

        unique_ptr<target_control> target_control_{};

        const session_active_marker session_active_{};

        session(tcp::socket socket, on_exit_type on_exit)
            : socket_(move(socket)), timer_(socket_.get_io_context()), on_exit_(on_exit)
        {
            cout << "Session started" << endl;
            try {
                target_control_.reset(new target_control());
            } catch (exception& e) {
                cerr << "Exception: " << e.what() << endl;
            }
        }
        ~session()
        {
            stop_program();
            cout << "Session stopped" << endl;
            if (on_exit_)
                on_exit_();
        }

        void start() { do_read(); }

        void set_timer()
        {
            auto self = shared_from_this();
            timer_.async_wait([this, self](error_code ec) {
                if (ec != asio::error::operation_aborted) {
                    asio::post(socket_.get_io_context(), [this]() { socket_.close(); });
                    cerr << "Session timed out!" << endl;
                } else if (!ec)
                    self->set_timer();
            });
        }

        bool is_executing()
        {
            if (programJob_.valid()) {
                if (programJob_.wait_for(chrono::seconds(0)) == future_status::timeout)
                    return true;
                programJob_.get();
            }
            return false;
        }

        void stop_program()
        {
            if (is_executing()) {
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

            programJob_ = async(launch::async, [this] {
                const running_program_marker running_program_marker_;
                (void)running_program_marker_;
                programStartTime_    = clock_type::now();
                auto currentStepTime = programStartTime_;
                for (const auto& current_step : program_) {
                    currentStepTime += current_step.time_to_execute_;
                    {
                        unique_lock<mutex> lock(mutex_);
                        if (cv_.wait_until(lock, currentStepTime, [&] { return stop_flag_; })) {
                            cout << "Program stopped!" << endl;
                            return;
                        }
                    }
                    if (current_step.fn_) {
                        auto t_relative =
                            chrono::duration_cast<chrono::milliseconds>(currentStepTime - programStartTime_).count();
                        cout << "T" << t_relative << ": ";
                        try {
                            current_step.fn_();
                        } catch (exception& e) {
                            cout << " Error: " << e.what();
                        }
                        cout << endl;
                    }
                }
                cout << "Program ended!" << endl;
            });
        }

        string parse_command(const string& s)
        {
            try {
                switch (s.front()) {
                case 'C':  // Clear program
                    stop_program();
                    program_.clear();
                    cout << "Program cleared!" << endl;
                    break;

                case 'T': {
                    auto ms = int(stof(s.substr(1)) * 1000);
                    program_.emplace_back(chrono::milliseconds(ms), function<void()>{});

                } break;

                case 'A':  // Play audio
                {
                    auto arg = s.substr(1);
                    if (arg.empty())
                        throw runtime_error("Syntax");

                    program_.emplace_back(chrono::milliseconds::zero(), [this, arg] {
                        cout << "Playing audio file '" << arg << "';";
                        if (!audioPlayCmdLinePrefix_.empty()) {
                            string cmdline = audioPlayCmdLinePrefix_;
                            size_t index   = cmdline.find("{f}");
                            if (index != string::npos)
                                cmdline.replace(index, arg.length(), arg);
                            thread([cmdline] { system(cmdline.c_str()); }).detach();
                        }

                    });
                } break;

                case 'M':  // Move target
                {
                    auto arg = stoi(s.substr(1));
                    program_.emplace_back(chrono::seconds::zero(), [this, arg] {
                        cout << "Moving target to position '" << arg << "';";
                        if (target_control_)
                            target_control_->move_target(!!arg);
                    });
                } break;

                case 'P':  // Play audio file directly
                {
                    if (is_executing())
                        throw runtime_error("Executing");

                    auto arg = s.substr(1);
                    if (arg.empty())
                        throw runtime_error("Syntax");

                    cout << "Playing audio file '" << arg << "' directly\n";
                    if (!audioPlayCmdLinePrefix_.empty()) {
                        string cmdline = audioPlayCmdLinePrefix_;
                        size_t index   = cmdline.find("{f}");
                        if (index != string::npos)
                            cmdline.replace(index, arg.length(), arg);
                        thread([cmdline] { system(cmdline.c_str()); }).detach();
                    }

                } break;

                case 'D':  // Move target directly
                    if (is_executing())
                        throw runtime_error("Executing");

                    if (target_control_) {
                        auto arg = stoi(s.substr(1));
                        cout << "Moving target to position '" << arg << "'" << endl;
                        target_control_->move_target(!!arg);
                    } else
                        throw runtime_error("Target");
                    break;

                case 'R':  // Run program
                    start_program();
                    break;

                case 'S':  // Stop program
                    stop_program();
                    break;

                case 'Q':  // Query state
                {
                    stringstream msg;
                    auto t_relative =
                        chrono::duration_cast<chrono::milliseconds>(clock_type::now() - programStartTime_).count();
                    auto t_total = chrono::milliseconds::zero();
                    for (auto& step : program_)
                        t_total += step.time_to_execute_;

                    msg << "EXEC=" << (is_executing() ? to_string(t_relative / 1000.0) : "") << "\r\n"
                        << "PROG=" << (!program_.empty() ? to_string(t_total.count() / 1000.0) : "") << "\r\n"
                        << "POS=" << (target_control_ ? to_string(int(target_control_->position())) : "") << "\r\n";
                    return msg.str();
                } break;

                case 'X':  // Exit, will disconnect the session immediately
                    throw client_exit();

                default:
                    throw runtime_error("UnknownCommand");
                }

                return move(string{});
            } catch (runtime_error& e) {
                throw e;
            } catch (const exception&) {
                throw runtime_error("Syntax");
            }
        }

        void do_read()
        {
            if (sessionTimeout_ > 0) {
                // Start watchdog timer
                timer_.cancel();
                timer_.expires_after(chrono::seconds(sessionTimeout_));
                set_timer();
            }

            auto self = shared_from_this();
            socket_.async_read_some(asio::buffer(data_, max_length), [this, self](error_code ec, size_t length) {
                if (!ec) {
                    try {
                        auto cmds = split_commands(move(string{data_.data(), length}));
                        if (!cmds.empty()) {
                            string reply;
                            for (auto& cmd : cmds)
                                reply += parse_command(cmd);

                            if (!reply.empty())
                                do_write(reply);
                            else
                                do_write("OK\r\n");
                        } else
                            do_read();
                    } catch (const client_exit&) {
                        timer_.cancel();
                    } catch (const exception& e) {
                        stringstream msg;
                        msg << "ERROR=" << e.what() << "\r\n";
                        do_write(msg.str());
                    }
                } else
                    timer_.cancel();
            });
        }

        void do_write(const string& msg)
        {
            size_t len = msg.copy(data_.data(), max_length);
            auto self  = shared_from_this();
            asio::async_write(socket_, asio::buffer(data_, len), [this, self](error_code ec, size_t /*length*/) {
                if (!ec)
                    do_read();
                else
                    timer_.cancel();
            });
        }
    };

    struct single_connection_server {
        tcp::acceptor acceptor_;
        const short port_;
        const server_ready_marker server_ready_;  // Used to light a LED when server is ready
        unique_ptr<button_handler> button_handler_;

        single_connection_server(asio::io_context& io_context, short port) : acceptor_(io_context), port_(port)
        {
            start_accept();
        }

        void start_accept()
        {
            if (!button_handler_)
                button_handler_ = make_unique<button_handler>();

            tcp::endpoint endpoint(tcp::v4(), port_);
            acceptor_.open(endpoint.protocol());
            acceptor_.bind(endpoint);
            acceptor_.listen(1);  // accept 1 connection at a time
            acceptor_.async_accept([this](error_code ec, tcp::socket socket) {
                if (!ec) {
                    stop_accept();
                    make_shared<session>(move(socket), [&] { start_accept(); })->start();
                } else
                    start_accept();

            });
        }

        void stop_accept()
        {
            button_handler_ = nullptr;
            acceptor_.close();
        }
    };

    struct broadcast_server {
        udp::endpoint sender_endpoint_;
        udp::socket socket_;
        asio::ip::address_v4 address_;
        const int port_;
        string token_;
        enum { max_length = 4096 };
        char data_[max_length];

        broadcast_server(asio::io_context& io_context, const string& token, asio::ip::address_v4 addr, int port)
            : socket_(io_context), token_(token), address_(addr), port_(port)
        {
            udp::endpoint listen_ep(addr, port_);
            socket_.open(listen_ep.protocol());
            socket_.set_option(udp::socket::reuse_address(true));
            socket_.set_option(udp::socket::broadcast(true));
            socket_.bind(listen_ep);
            start_async_receive();
        }

        void start_async_receive()
        {
            socket_.async_receive_from(
                asio::buffer(data_, max_length),
                sender_endpoint_,
                bind(&broadcast_server::handle_receive_from, this, placeholders::_1, placeholders::_2));
        }

        void handle_receive_from(const asio::error_code& error, size_t bytes_recvd)
        {
            if (!error) {
                auto s = string{data_, data_ + bytes_recvd};
                if (s.find(token_) != string::npos) {
                    // Token is found!
                    stringstream ss;
                    ss << "IP:" << address_.to_string() << ":" << port_ << "\r\n";
                    socket_.send_to(asio::buffer(ss.str()), sender_endpoint_);
                    cout << "Token intercepted, sent address to " << sender_endpoint_.address().to_string() << endl;
                }
            }
            start_async_receive();
        }
    };
}

int main(int argc, char* argv[])
{
    CLI::App app{"Target daemon"};

    try {
        string token("{BC5C0A2F-7091-4254-B576-7F0E2F0441A6}");
        int port = 7777;

        app.add_option("--port", port, "Port to listen upon, default is 7777");
        app.add_option("--play-cmd", audioPlayCmdLinePrefix_, "Audio play cmd line prefix")->required(true);
        app.add_option(
            "--timeout",
            sessionTimeout_,
            "Watchdog timeout in seconds (default " + to_string(sessionTimeout_) + "), zero disables watchdog",
            true);
        app.add_option("--token", token, "Token to listen for on broadcast address, default is '" + token + "'");

        CLI11_PARSE(app, argc, argv);

        cout << "Audio play prefix: '" << audioPlayCmdLinePrefix_ << "'" << endl;

        asio::io_context io_context;
        single_connection_server s(io_context, port);
        cout << "Daemon started listening on port " << port << endl;

        // Start up broadcast receiver
        vector<unique_ptr<broadcast_server>> bc_servers;
        auto ip_addresses = utility::get_interface_addresses();
        for (const auto& ip : ip_addresses) {
            if (!ip.is_v4())
                continue;

            if (ip.to_v4().is_loopback())
                continue;

            auto p = make_unique<broadcast_server>(io_context, token, ip.to_v4(), port);
            bc_servers.push_back(move(p));
            cout << "Broadcast server listening on token '" << token << "' on IP " << ip.to_v4().to_string() << endl;
        }

        io_context.run();
    } catch (exception& e) {
        cerr << "Exception: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
