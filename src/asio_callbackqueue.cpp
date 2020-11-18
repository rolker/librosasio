#include <iostream>
#include <ros/init.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>

#include "librosasio/asio_callbackqueue.h"

namespace ros
{
extern CallbackQueuePtr g_global_queue;
}

int AsioCallbackQueue::m_sigtermFd[2];

AsioCallbackQueue::AsioCallbackQueue(boost::asio::io_service &io_context)
    : m_io_context(io_context), m_sigTermStream(io_context)
{
    if(::socketpair(AF_UNIX, SOCK_STREAM, 0, AsioCallbackQueue::m_sigtermFd))
    {
        std::cerr << "Couldn't create TERM socketpair\n";
        ::exit(1);
    }

    m_sigTermStream.assign(m_sigtermFd[1]);
    m_sigTermStream.async_read_some(
        boost::asio::buffer(&m_socketReadBuffer, 1),
        [this](const boost::system::error_code& error, std::size_t bytes_received) {
            handleSigTerm(error, bytes_received);
        });

    struct sigaction term;
    term.sa_handler = AsioCallbackQueue::termSignalHandler;
    ::sigemptyset(&term.sa_mask);
    term.sa_flags |= SA_RESTART;
    if(::sigaction(SIGINT, &term, 0) > 0)
    {
        std::cerr << "Can't perform sigaction for INT Signal.\n";
        ::exit(1);
    }
    if(::sigaction(SIGTERM, &term, 0) > 0)
    {
        std::cerr << "Can't perform sigaction for TERM Signal.\n";
        ::exit(1);
    }
}

void AsioCallbackQueue::addCallback(const ros::CallbackInterfacePtr &callback, uint64_t owner_id)
{
  ros::CallbackQueue::addCallback(callback, owner_id);
#if BOOST_VERSION <= 106501
  m_io_context.post([this]
#else
  boost::asio::post(m_io_context, [this]
#endif
                    {
                      // This post() will be called once per callback to process so only process one at a time
                      callOne();
                    });
}

void AsioCallbackQueue::replaceGlobalQueue(boost::asio::io_service &io_context)
{
  ros::g_global_queue.reset(new AsioCallbackQueue(io_context));
}

void AsioCallbackQueue::termSignalHandler(int signal_number)
{
    // This method is called in the POSIX "signal" context.
    // Only reentrant methods can be called from here!
    // eg. any call to new or malloc could cause deadlocks

    // set flag to shutdown ROS ; this is needed if for example a nodehandle is waiting
    // for the rosmaster
    ros::requestShutdown();

    // send the signal number on the socket to notify Boost that a shutdown is requested
    char receivedSignal = static_cast<char>(signal_number);
    const int ret = ::write(AsioCallbackQueue::m_sigtermFd[0], &receivedSignal,
                      sizeof(receivedSignal));
    (void)ret;
}

void AsioCallbackQueue::handleSigTerm(const boost::system::error_code& error,
                                      std::size_t bytes_received)
{
    if(!error)
    {
        const auto signal_str = [this]() -> std::string {
            switch(m_socketReadBuffer)
            {
            case SIGINT: return "SIGINT";
            case SIGTERM: return "SIGTERM";
            }
            return std::to_string(m_socketReadBuffer);
        }();
        std::cout << "Received signal " << signal_str << '\n';

        m_io_context.stop();
    }
}
