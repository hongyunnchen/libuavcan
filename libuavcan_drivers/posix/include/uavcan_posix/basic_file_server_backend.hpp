/****************************************************************************
*
*   Copyright (c) 2015 PX4 Development Team. All rights reserved.
*      Author: Pavel Kirienko <pavel.kirienko@gmail.com>
*              David Sidrane <david_s5@usa.net>
*
****************************************************************************/

#ifndef UAVCAN_POSIX_BASIC_FILE_SERVER_BACKEND_HPP_INCLUDED
#define UAVCAN_POSIX_BASIC_FILE_SERVER_BACKEND_HPP_INCLUDED

#include <sys/stat.h>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>

#include <uavcan/node/timer.hpp>
#include <uavcan/data_type.hpp>
#include <uavcan/protocol/file/Error.hpp>
#include <uavcan/protocol/file/EntryType.hpp>
#include <uavcan/protocol/file/Read.hpp>
#include <uavcan/protocol/file_server.hpp>
#include <uavcan/data_type.hpp>

namespace uavcan_posix
{
/**
 * This interface implements a POSIX compliant IFileServerBackend interface
 */
class BasicFileSeverBackend : public uavcan::IFileServerBackend
{
    enum { FilePermissions = 438 };   ///< 0o666

protected:
    class FDCacheBase
    {
    public:
        FDCacheBase() { }
        virtual ~FDCacheBase() { }

        virtual int open(const char* path, int oflags)
        {
            using namespace std;

            return ::open(path, oflags);
        }

        virtual int close(int fd, bool done = true)
        {
            (void)done;
            using namespace std;

            return ::close(fd);
        }

        virtual void init() { }
    };

    FDCacheBase fallback_;

    class FDCache : public FDCacheBase, protected uavcan::TimerBase
    {
        /* Age in Seconds an entry will stay in the cache if not accessed. */

        enum { MaxAgeSeconds = 7 };

        /* Rate in Seconds that the cache will be flushed of stale entries. */

        enum { GarbageCollectionSeconds = 60 };

        class FDCacheItem
        {
            friend FDCache;

            FDCacheItem* next_;
            time_t last_access_;
            int fd_;
            int oflags_;
            const char* path_;

        public:
            enum { InvalidFD = -1 };

            FDCacheItem()
                : next_(NULL),
                last_access_(0),
                fd_(InvalidFD),
                oflags_(0),
                path_(0)
            { }

            FDCacheItem(int fd, const char* path, int oflags)
                : next_(NULL),
                last_access_(0),
                fd_(fd),
                oflags_(oflags),
                path_(strdup(path))
            { }

            ~FDCacheItem()
            {
                if (valid())
                {
                    delete path_;
                }
            }

            bool valid() const
            {
                return path_ != NULL;
            }

            int getFD() const
            {
                return fd_;
            }

            time_t getAccess() const
            {
                return last_access_;
            }

            time_t acessed()
            {
                last_access_ = time(NULL);
                return getAccess();
            }

            void expire()
            {
                last_access_ = 0;
            }

            bool expired() const
            {
                return 0 == last_access_ || (time(NULL) - last_access_) > MaxAgeSeconds;
            }

            bool equals(const char* path, int oflags) const
            {
                return oflags_ == oflags && 0 == ::strcmp(path, path_);
            }

            bool equals(int fd) const
            {
                return fd_ == fd;
            }
        };

        FDCacheItem* head_;

        FDCacheItem* find(const char* path, int oflags)
        {
            for (FDCacheItem* pi = head_; pi; pi = pi->next_)
            {
                if (pi->equals(path, oflags))
                {
                    return pi;
                }
            }
            return NULL;
        }

        FDCacheItem* find(int fd)
        {
            for (FDCacheItem* pi = head_; pi; pi = pi->next_)
            {
                if (pi->equals(fd))
                {
                    return pi;
                }
            }
            return NULL;
        }

        FDCacheItem* add(FDCacheItem* pi)
        {
            pi->next_ = head_;
            head_ = pi;
            pi->acessed();
            return pi;
        }

        void removeExpired(FDCacheItem** pi)
        {
            while (*pi)
            {
                if ((*pi)->expired())
                {
                    FDCacheItem* next = (*pi)->next_;
                    (void)FDCacheBase::close((*pi)->fd_);
                    delete (*pi);
                    *pi = next;
                    continue;
                }
                pi = &(*pi)->next_;
            }
        }

        void remove(FDCacheItem* pi, bool done)
        {
            if (done)
            {
                pi->expire();
            }
            removeExpired(&head_);
        }

        void clear()
        {
            FDCacheItem* tmp;
            for (FDCacheItem* pi = head_; pi; pi = tmp)
            {
                tmp = pi->next_;
                (void)FDCacheBase::close(pi->fd_);
                delete pi;
            }
        }

        /* Removed stale entries. In the normal case a node will read the
         * complete contents of a file and the read of the last block will
         * cause the method remove() to be invoked with done true. Thereby
         * flushing the entry from the cache. But if the node does not
         * stay the course of the read, it may leave a dangling entry.
         * This call back handles the garbage collection.
         */
        virtual void handleTimerEvent(const uavcan::TimerEvent& event)
        {
            removeExpired(&head_);
        }

    public:
        FDCache(uavcan::INode& node) :
            TimerBase(node),
            head_(NULL)
        { }

        virtual ~FDCache()
        {
            stop();
            clear();
        }

        virtual void init()
        {
            startPeriodic(uavcan::MonotonicDuration::fromMSec(GarbageCollectionSeconds * 1000));
        }

        virtual int open(const char* path, int oflags)
        {
            int fd = FDCacheItem::InvalidFD;

            FDCacheItem* pi = find(path, oflags);

            if (pi != NULL)
            {
                pi->acessed();
            }
            else
            {
                fd = FDCacheBase::open(path, oflags);
                if (fd < 0)
                {
                    return fd;
                }

                /* Allocate and clone path */

                pi = new FDCacheItem(fd, path, oflags);

                /* Allocation worked but check clone */

                if (pi && !pi->valid())
                {
                    /* Allocation worked but clone or path failed */

                    delete pi;
                    pi = NULL;
                }

                if (pi == NULL)
                {
                    /*
                     * If allocation fails no harm just can not cache it
                     * return open fd
                     */

                    return fd;
                }
                /* add new */
                add(pi);
            }
            return pi->getFD();
        }

        virtual int close(int fd, bool done)
        {
            FDCacheItem* pi = find(fd);
            if (pi == NULL)
            {
                /*
                 * If not found just close it
                 */
                return FDCacheBase::close(fd);
            }
            remove(pi, done);
            return 0;
        }
    };

    FDCacheBase* fdcache_;
    uavcan::INode& node_;

    FDCacheBase& getFDCache()
    {
        if (fdcache_ == NULL)
        {
            fdcache_ = new FDCache(node_);

            if (fdcache_ == NULL)
            {
                fdcache_ = &fallback_;
            }

            fdcache_->init();
        }
        return *fdcache_;
    }

    /**
     * Back-end for uavcan.protocol.file.GetInfo.
     * Implementation of this method is required.
     * On success the method must return zero.
     */
    virtual int16_t getInfo(const Path& path, uint64_t& out_size, EntryType& out_type)
    {
        int rv = uavcan::protocol::file::Error::INVALID_VALUE;

        if (path.size() > 0)
        {
            using namespace std;

            struct stat sb;

            rv = stat(path.c_str(), &sb);

            if (rv < 0)
            {
                rv = errno;
            }
            else
            {
                rv = 0;
                out_size = sb.st_size;
                out_type.flags = uavcan::protocol::file::EntryType::FLAG_READABLE;
                if (S_ISDIR(sb.st_mode))
                {
                    out_type.flags |= uavcan::protocol::file::EntryType::FLAG_DIRECTORY;
                }
                else if (S_ISREG(sb.st_mode))
                {
                    out_type.flags |= uavcan::protocol::file::EntryType::FLAG_FILE;
                }
                // TODO Using fixed flag FLAG_READABLE until we add file permission checks to return actual value.
            }
        }
        return rv;
    }

    /**
     * Back-end for uavcan.protocol.file.Read.
     * Implementation of this method is required.
     * @ref inout_size is set to @ref ReadSize; read operation is required to return exactly this amount, except
     * if the end of file is reached.
     * On success the method must return zero.
     */
    virtual int16_t read(const Path& path, const uint64_t offset, uint8_t* out_buffer, uint16_t& inout_size)
    {
        int rv = uavcan::protocol::file::Error::INVALID_VALUE;

        if (path.size() > 0)
        {
            FDCacheBase& cache = getFDCache();
            int fd = cache.open(path.c_str(), O_RDONLY);

            if (fd < 0)
            {
                rv = errno;
            }
            else
            {
                rv = ::lseek(fd, offset, SEEK_SET);

                ssize_t len = 0;

                if (rv < 0)
                {
                    rv = errno;
                }
                else
                {
                    // TODO use a read at offset to fill on EAGAIN
                    len = ::read(fd, out_buffer, inout_size);

                    if (len < 0)
                    {
                        rv = errno;
                    }
                    else
                    {
                        rv = 0;
                    }
                }

                (void)cache.close(fd, rv != 0 || len != inout_size);
                inout_size = len;
            }
        }
        return rv;
    }

public:
    BasicFileSeverBackend(uavcan::INode& node) :
        fdcache_(NULL),
        node_(node)
    { }

    ~BasicFileSeverBackend()
    {
        if (fdcache_ != &fallback_)
        {
            delete fdcache_;
            fdcache_ = NULL;
        }
    }
};
}

#endif // Include guard
