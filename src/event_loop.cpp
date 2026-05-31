#include "rpc/event_loop.h"
#include "rpc/event_handler.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

namespace rpc {

EventLoop::EventLoop() {
    epfd_ = epoll_create1(0);
    if (epfd_ < 0) {
        throw std::runtime_error("epoll_create1() failed");
    }

    // 创建 eventfd 用于 Stop() 唤醒 epoll_wait
    wakeup_fd_ = eventfd(0, EFD_NONBLOCK);
    if (wakeup_fd_ < 0) {
        close(epfd_);
        throw std::runtime_error("eventfd() failed");
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wakeup_fd_;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, wakeup_fd_, &ev);
}

EventLoop::~EventLoop() {
    if (epfd_ >= 0) close(epfd_);
    if (wakeup_fd_ >= 0) close(wakeup_fd_);
}

void EventLoop::Run() {
    running_ = true;

    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    while (running_) {
        int n = epoll_wait(epfd_, events, kMaxEvents, -1);

        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            // wakeup_fd_ 事件：仅消费数据，不触发 handler
            if (fd == wakeup_fd_) {
                uint64_t dummy;
                read(wakeup_fd_, &dummy, sizeof(dummy));
                continue;
            }

            auto it = handlers_.find(fd);
            if (it == handlers_.end()) continue;

            EventHandler* handler = it->second.get();
            uint32_t ev = events[i].events;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                handler->OnClose();
            } else {
                if (ev & EPOLLIN)  handler->OnRead();
                if (ev & EPOLLOUT) handler->OnWrite();
            }
        }
    }
}

void EventLoop::Stop() {
    running_ = false;
    // 向 wakeup_fd_ 写入 1 字节，唤醒正在阻塞的 epoll_wait
    uint64_t one = 1;
    write(wakeup_fd_, &one, sizeof(one));
}

void EventLoop::Register(std::unique_ptr<EventHandler> handler, uint32_t events) {
    int fd = handler->GetFd();

    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::runtime_error("epoll_ctl(ADD) failed");
    }

    handlers_[fd] = std::move(handler);
}

void EventLoop::Unregister(int fd) {
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    handlers_.erase(fd);
}

} // namespace rpc