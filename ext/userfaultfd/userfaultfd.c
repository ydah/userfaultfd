#include "ruby.h"
#include "ruby/thread.h"
#include "extconf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
# include <sys/ioctl.h>
# include <sys/syscall.h>
# include "compat.h"
#endif

typedef struct {
    int fd;
    pid_t pid;
    uint64_t features;
    uint64_t enabled_features;
    uint64_t ioctls;
    uintptr_t registered_start;
    size_t registered_length;
    int backend;
} uffd_data_t;

typedef struct {
    void *address;
    size_t size;
    size_t page_size;
    int busy;
    int shared;
} region_data_t;

typedef struct {
    VALUE owner;
    VALUE source;
    pid_t pid;
    pthread_t thread;
    int stop_pipe[2];
    int source_fd;
    int started;
    volatile int running;
    int joined;
    volatile int error;
    int mode;
    int uffd_fd;
    uintptr_t start;
    size_t length;
    size_t page_size;
    const void *source_address;
} native_handler_data_t;

#define EVENT_READER_MAX_FDS 64
typedef struct {
    VALUE owner;
    pid_t pid;
    pthread_t thread;
    int stop_pipe[2];
    int output_pipe[2];
    int fds[EVENT_READER_MAX_FDS];
    unsigned char owned[EVENT_READER_MAX_FDS];
    size_t fd_count;
    int started;
    int joined;
    volatile int running;
    volatile int error;
} event_reader_data_t;

static VALUE cUserfaultFD;
static VALUE cRegion;
static VALUE cFault;
static VALUE cForkEvent;
static VALUE cRemapEvent;
static VALUE cRemoveEvent;
static VALUE cUnmapEvent;
static VALUE cNativeHandler;
static VALUE cEventReader;
static VALUE eError;
static VALUE eUnsupportedError;

static ID id_size;
static ID id_shared;
static ID id_huge;
static ID id_features;
static ID id_mode;
static ID id_enabled;
static ID id_wake;
static ID id_offset;

void userfaultfd_define_constants(VALUE klass);

static void native_handler_stop_and_join(native_handler_data_t *data);
static void event_reader_stop_and_join(event_reader_data_t *data);

static void
uffd_free(void *ptr)
{
    uffd_data_t *data = ptr;
    if (data->fd >= 0) close(data->fd);
    xfree(data);
}

static size_t
uffd_memsize(const void *ptr)
{
    return ptr ? sizeof(uffd_data_t) : 0;
}

static const rb_data_type_t uffd_type = {
    "UserfaultFD",
    {NULL, uffd_free, uffd_memsize, NULL},
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY
};

static void
region_free(void *ptr)
{
    region_data_t *data = ptr;
    if (data->address != MAP_FAILED) munmap(data->address, data->size);
    xfree(data);
}

static size_t
region_memsize(const void *ptr)
{
    return ptr ? sizeof(region_data_t) : 0;
}

static const rb_data_type_t region_type = {
    "UserfaultFD::Region",
    {NULL, region_free, region_memsize, NULL},
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY
};

static void
native_handler_mark(void *ptr)
{
    native_handler_data_t *data = ptr;
    rb_gc_mark(data->owner);
    rb_gc_mark(data->source);
}

static void
native_handler_free(void *ptr)
{
    native_handler_data_t *data = ptr;
    native_handler_stop_and_join(data);
    if (data->stop_pipe[0] >= 0) close(data->stop_pipe[0]);
    if (data->stop_pipe[1] >= 0) close(data->stop_pipe[1]);
    if (data->source_fd >= 0) close(data->source_fd);
    xfree(data);
}

static size_t
native_handler_memsize(const void *ptr)
{
    return ptr ? sizeof(native_handler_data_t) : 0;
}

static const rb_data_type_t native_handler_type = {
    "UserfaultFD::NativeHandler",
    {native_handler_mark, native_handler_free, native_handler_memsize, NULL},
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static VALUE
native_handler_alloc(VALUE klass)
{
    native_handler_data_t *data;
    VALUE object = TypedData_Make_Struct(
        klass, native_handler_data_t, &native_handler_type, data
    );
    data->owner = Qnil;
    data->source = Qnil;
    data->pid = getpid();
    data->stop_pipe[0] = -1;
    data->stop_pipe[1] = -1;
    data->source_fd = -1;
    return object;
}

static void
event_reader_mark(void *ptr)
{
    event_reader_data_t *data = ptr;
    rb_gc_mark(data->owner);
}

static void
event_reader_free(void *ptr)
{
    event_reader_data_t *data = ptr;
    size_t i;
    event_reader_stop_and_join(data);
    for (i = 0; i < data->fd_count; i++) {
        if (data->owned[i] && data->fds[i] >= 0) close(data->fds[i]);
    }
    if (data->stop_pipe[0] >= 0) close(data->stop_pipe[0]);
    if (data->stop_pipe[1] >= 0) close(data->stop_pipe[1]);
    if (data->output_pipe[0] >= 0) close(data->output_pipe[0]);
    if (data->output_pipe[1] >= 0) close(data->output_pipe[1]);
    xfree(data);
}

static void
event_reader_close_owned(event_reader_data_t *data)
{
    size_t i;
    for (i = 0; i < data->fd_count; i++) {
        if (data->owned[i] && data->fds[i] >= 0) {
            close(data->fds[i]);
            data->fds[i] = -1;
        }
    }
}

static size_t
event_reader_memsize(const void *ptr)
{
    return ptr ? sizeof(event_reader_data_t) : 0;
}

static const rb_data_type_t event_reader_type = {
    "UserfaultFD::EventReader",
    {event_reader_mark, event_reader_free, event_reader_memsize, NULL},
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static VALUE
event_reader_alloc(VALUE klass)
{
    event_reader_data_t *data;
    VALUE object = TypedData_Make_Struct(
        klass, event_reader_data_t, &event_reader_type, data
    );
    size_t i;
    data->owner = Qnil;
    data->pid = getpid();
    data->stop_pipe[0] = data->stop_pipe[1] = -1;
    data->output_pipe[0] = data->output_pipe[1] = -1;
    for (i = 0; i < EVENT_READER_MAX_FDS; i++) data->fds[i] = -1;
    return object;
}

static VALUE
uffd_alloc(VALUE klass)
{
    uffd_data_t *data;
    VALUE object = TypedData_Make_Struct(klass, uffd_data_t, &uffd_type, data);
    data->fd = -1;
    data->pid = getpid();
    return object;
}

static VALUE
region_alloc(VALUE klass)
{
    region_data_t *data;
    VALUE object = TypedData_Make_Struct(klass, region_data_t, &region_type, data);
    data->address = MAP_FAILED;
    data->page_size = (size_t)sysconf(_SC_PAGESIZE);
    return object;
}

static uffd_data_t *
get_uffd(VALUE self)
{
    uffd_data_t *data;
    TypedData_Get_Struct(self, uffd_data_t, &uffd_type, data);
    if (data->fd < 0) rb_raise(eError, "closed userfaultfd");
    if (data->pid != getpid()) rb_raise(eError, "userfaultfd cannot be used after fork");
    return data;
}

static region_data_t *
get_region(VALUE object)
{
    region_data_t *data;
    TypedData_Get_Struct(object, region_data_t, &region_type, data);
    if (data->address == MAP_FAILED) rb_raise(eError, "unmapped region");
    return data;
}

static void raise_unsupported(const char *message) __attribute__((noreturn));

static void
raise_unsupported(const char *message)
{
    rb_raise(eUnsupportedError, "%s", message);
}

#ifdef __linux__
static int
open_uffd(int *backend)
{
    int dev = open("/dev/userfaultfd", O_RDWR | O_CLOEXEC);
    if (dev >= 0) {
        int fd = ioctl(dev, USERFAULTFD_IOC_NEW,
                       O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        int saved_errno = errno;
        close(dev);
        if (fd >= 0) {
            *backend = 1;
            return fd;
        }
        errno = saved_errno;
    }

# ifdef SYS_userfaultfd
    {
        int fd = (int)syscall(SYS_userfaultfd,
                              O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (fd >= 0) {
            *backend = 2;
            return fd;
        }
    }
# else
    errno = ENOSYS;
# endif
    return -1;
}

static int
query_api(int fd, uint64_t requested, uint64_t *features, uint64_t *ioctls)
{
    struct uffdio_api api;
    memset(&api, 0, sizeof(api));
    api.api = UFFD_API;
    api.features = requested;
    if (ioctl(fd, UFFDIO_API, &api) < 0) return -1;
    *features = api.features;
    *ioctls = api.ioctls;
    return 0;
}

struct feature_name {
    const char *name;
    uint64_t value;
};

static const struct feature_name feature_names[] = {
#ifdef UFFD_FEATURE_PAGEFAULT_FLAG_WP
    {"pagefault_flag_wp", UFFD_FEATURE_PAGEFAULT_FLAG_WP},
#endif
#ifdef UFFD_FEATURE_EVENT_FORK
    {"event_fork", UFFD_FEATURE_EVENT_FORK},
#endif
#ifdef UFFD_FEATURE_EVENT_REMAP
    {"event_remap", UFFD_FEATURE_EVENT_REMAP},
#endif
#ifdef UFFD_FEATURE_EVENT_REMOVE
    {"event_remove", UFFD_FEATURE_EVENT_REMOVE},
#endif
#ifdef UFFD_FEATURE_EVENT_UNMAP
    {"event_unmap", UFFD_FEATURE_EVENT_UNMAP},
#endif
#ifdef UFFD_FEATURE_MISSING_HUGETLBFS
    {"missing_hugetlbfs", UFFD_FEATURE_MISSING_HUGETLBFS},
#endif
#ifdef UFFD_FEATURE_MISSING_SHMEM
    {"missing_shmem", UFFD_FEATURE_MISSING_SHMEM},
#endif
#ifdef UFFD_FEATURE_SIGBUS
    {"sigbus", UFFD_FEATURE_SIGBUS},
#endif
#ifdef UFFD_FEATURE_THREAD_ID
    {"thread_id", UFFD_FEATURE_THREAD_ID},
#endif
#ifdef UFFD_FEATURE_MINOR_HUGETLBFS
    {"minor_hugetlbfs", UFFD_FEATURE_MINOR_HUGETLBFS},
#endif
#ifdef UFFD_FEATURE_MINOR_SHMEM
    {"minor_shmem", UFFD_FEATURE_MINOR_SHMEM},
#endif
#ifdef UFFD_FEATURE_EXACT_ADDRESS
    {"exact_address", UFFD_FEATURE_EXACT_ADDRESS},
#endif
#ifdef UFFD_FEATURE_WP_HUGETLBFS_SHMEM
    {"wp_hugetlbfs_shmem", UFFD_FEATURE_WP_HUGETLBFS_SHMEM},
#endif
#ifdef UFFD_FEATURE_WP_UNPOPULATED
    {"wp_unpopulated", UFFD_FEATURE_WP_UNPOPULATED},
#endif
#ifdef UFFD_FEATURE_WP_ASYNC
    {"wp_async", UFFD_FEATURE_WP_ASYNC},
#endif
#ifdef UFFD_FEATURE_POISON
    {"poison", UFFD_FEATURE_POISON},
#endif
#ifdef UFFD_FEATURE_MOVE
    {"move", UFFD_FEATURE_MOVE},
#endif
    {NULL, 0}
};

static uint64_t
feature_mask(VALUE names)
{
    uint64_t mask = 0;
    long i;
    Check_Type(names, T_ARRAY);
    for (i = 0; i < RARRAY_LEN(names); i++) {
        VALUE name = rb_ary_entry(names, i);
        const char *value;
        const struct feature_name *entry;
        if (!SYMBOL_P(name)) rb_raise(rb_eArgError, "features must contain symbols");
        value = rb_id2name(SYM2ID(name));
        for (entry = feature_names; entry->name; entry++) {
            if (strcmp(value, entry->name) == 0) {
                mask |= entry->value;
                break;
            }
        }
        if (!entry->name) rb_raise(rb_eArgError, "unknown feature: %s", value);
    }
    return mask;
}

static VALUE
features_to_array(uint64_t mask)
{
    VALUE result = rb_ary_new();
    const struct feature_name *entry;
    for (entry = feature_names; entry->name; entry++) {
        if ((mask & entry->value) != 0)
            rb_ary_push(result, ID2SYM(rb_intern(entry->name)));
    }
    return result;
}
#endif

static VALUE
uffd_initialize(int argc, VALUE *argv, VALUE self)
{
    VALUE kwargs = Qnil;
    VALUE values[1] = {Qundef};
    uint64_t requested = 0;
    int features_given = 0;
    uffd_data_t *data;

    rb_scan_args(argc, argv, "0:", &kwargs);
    if (!NIL_P(kwargs)) {
        ID keys[] = {id_features};
        rb_get_kwargs(kwargs, keys, 0, 1, values);
        if (values[0] != Qundef && !NIL_P(values[0])) {
            features_given = 1;
#ifdef __linux__
            requested = feature_mask(values[0]);
#else
            (void)values;
#endif
        }
    }
    TypedData_Get_Struct(self, uffd_data_t, &uffd_type, data);

#ifdef __linux__
    {
        int probe_backend = 0;
        int probe_fd = open_uffd(&probe_backend);
        uint64_t available = 0, ignored_ioctls = 0;
        int saved_errno;
        if (probe_fd < 0) {
            if (errno == ENOSYS || errno == ENODEV || errno == EOPNOTSUPP)
                raise_unsupported("userfaultfd is not supported by this kernel");
            rb_syserr_fail(errno, "userfaultfd");
        }
        if (query_api(probe_fd, 0, &available, &ignored_ioctls) < 0) {
            saved_errno = errno;
            close(probe_fd);
            if (saved_errno == EINVAL || saved_errno == ENOTTY)
                raise_unsupported("UFFDIO_API is not supported by this kernel");
            rb_syserr_fail(saved_errno, "UFFDIO_API");
        }
        close(probe_fd);
#ifdef UFFD_FEATURE_EVENT_FORK
        if (!features_given) requested = available & UFFD_FEATURE_EVENT_FORK;
#else
        (void)features_given;
#endif
        if ((requested & available) != requested)
            raise_unsupported("requested userfaultfd features are unavailable");

open_production_fd:
        data->fd = open_uffd(&data->backend);
        if (data->fd < 0) rb_syserr_fail(errno, "userfaultfd");
        if (query_api(data->fd, requested, &data->features, &data->ioctls) < 0) {
            saved_errno = errno;
            close(data->fd);
            data->fd = -1;
            if (!features_given && requested && saved_errno == EPERM) {
                requested = 0;
                goto open_production_fd;
            }
            if (saved_errno == EINVAL)
                raise_unsupported("requested userfaultfd features were rejected");
            rb_syserr_fail(saved_errno, "UFFDIO_API");
        }
        data->enabled_features = requested;
    }
#else
    (void)requested;
    (void)features_given;
    raise_unsupported("userfaultfd is only available on Linux");
#endif
    return self;
}

static VALUE
uffd_supported(VALUE klass)
{
    (void)klass;
#ifdef __linux__
    {
        int backend = 0;
        int fd = open_uffd(&backend);
        uint64_t features, ioctls;
        if (fd < 0) return Qfalse;
        if (query_api(fd, 0, &features, &ioctls) < 0) {
            close(fd);
            return Qfalse;
        }
        close(fd);
        return Qtrue;
    }
#else
    return Qfalse;
#endif
    return Qnil;
}

static VALUE
uffd_features(VALUE klass)
{
    (void)klass;
#ifdef __linux__
    {
        int backend = 0;
        int fd = open_uffd(&backend);
        uint64_t features, ioctls;
        if (fd < 0) return rb_ary_new();
        if (query_api(fd, 0, &features, &ioctls) < 0) {
            close(fd);
            return rb_ary_new();
        }
        close(fd);
        return features_to_array(features);
    }
#else
    return rb_ary_new();
#endif
}

static VALUE
uffd_instance_features(VALUE self)
{
    uffd_data_t *data = get_uffd(self);
#ifdef __linux__
    return features_to_array(data->features);
#else
    (void)data;
    return rb_ary_new();
#endif
}

static VALUE
uffd_enabled_features(VALUE self)
{
    uffd_data_t *data = get_uffd(self);
#ifdef __linux__
    return features_to_array(data->enabled_features);
#else
    (void)data;
    return rb_ary_new();
#endif
}

static VALUE
uffd_backend(VALUE self)
{
    uffd_data_t *data = get_uffd(self);
    return ID2SYM(rb_intern(data->backend == 1 ? "device" : "syscall"));
}

static VALUE
uffd_fileno(VALUE self)
{
    return INT2NUM(get_uffd(self)->fd);
}

static VALUE
uffd_close(VALUE self)
{
    uffd_data_t *data;
    TypedData_Get_Struct(self, uffd_data_t, &uffd_type, data);
    if (data->fd >= 0) {
        close(data->fd);
        data->fd = -1;
    }
    return Qnil;
}

static VALUE
uffd_closed_p(VALUE self)
{
    uffd_data_t *data;
    TypedData_Get_Struct(self, uffd_data_t, &uffd_type, data);
    return data->fd < 0 ? Qtrue : Qfalse;
}

static VALUE
region_initialize(int argc, VALUE *argv, VALUE self)
{
    VALUE kwargs;
    VALUE values[3] = {Qundef, Qundef, Qundef};
    ID keys[] = {id_size, id_shared, id_huge};
    region_data_t *data;
    size_t size;
    int flags;

    rb_scan_args(argc, argv, "0:", &kwargs);
    rb_get_kwargs(kwargs, keys, 1, 2, values);
    TypedData_Get_Struct(self, region_data_t, &region_type, data);
    size = NUM2SIZET(values[0]);
    if (size == 0 || size % data->page_size != 0)
        rb_raise(rb_eArgError, "size must be a positive multiple of page_size");

    flags = (values[1] == Qfalse ? MAP_PRIVATE : MAP_SHARED) | MAP_ANONYMOUS;
    data->shared = values[1] != Qfalse;
    if (values[2] == Qtrue) {
#ifdef MAP_HUGETLB
        flags |= MAP_HUGETLB;
#else
        rb_raise(eUnsupportedError, "huge pages are unavailable on this platform");
#endif
    }
    data->address = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (data->address == MAP_FAILED) rb_syserr_fail(errno, "mmap");
    data->size = size;
    return self;
}

static VALUE
region_address(VALUE self)
{
    return ULL2NUM((uintptr_t)get_region(self)->address);
}

static VALUE
region_size(VALUE self)
{
    return SIZET2NUM(get_region(self)->size);
}

static VALUE
region_page_size(VALUE self)
{
    region_data_t *data;
    TypedData_Get_Struct(self, region_data_t, &region_type, data);
    return SIZET2NUM(data->page_size);
}

static VALUE
region_unmap(VALUE self)
{
    region_data_t *data;
    TypedData_Get_Struct(self, region_data_t, &region_type, data);
    if (data->address == MAP_FAILED) return Qnil;
    if (data->busy) rb_raise(eError, "region is in use");
    if (munmap(data->address, data->size) < 0) rb_syserr_fail(errno, "munmap");
    data->address = MAP_FAILED;
    return Qnil;
}

static VALUE
region_unmapped_p(VALUE self)
{
    region_data_t *data;
    TypedData_Get_Struct(self, region_data_t, &region_type, data);
    return data->address == MAP_FAILED ? Qtrue : Qfalse;
}

struct copy_args {
    void *destination;
    const void *source;
    size_t length;
};

static void *
without_gvl_memcpy(void *ptr)
{
    struct copy_args *args = ptr;
    memcpy(args->destination, args->source, args->length);
    return NULL;
}

static void
validate_range(region_data_t *data, size_t offset, size_t length)
{
    if (offset > data->size || length > data->size - offset)
        rb_raise(rb_eRangeError, "range is outside region");
}

static VALUE
region_read(VALUE self, VALUE offset_value, VALUE length_value)
{
    region_data_t *data = get_region(self);
    size_t offset = NUM2SIZET(offset_value);
    size_t length = NUM2SIZET(length_value);
    char *buffer;
    VALUE result;
    struct copy_args args;
    validate_range(data, offset, length);
    buffer = xmalloc(length ? length : 1);
    args.destination = buffer;
    args.source = (char *)data->address + offset;
    args.length = length;
    data->busy++;
    rb_thread_call_without_gvl(without_gvl_memcpy, &args, RUBY_UBF_IO, NULL);
    data->busy--;
    result = rb_str_new(buffer, (long)length);
    xfree(buffer);
    return result;
}

static VALUE
region_write(VALUE self, VALUE offset_value, VALUE string)
{
    region_data_t *data = get_region(self);
    size_t offset = NUM2SIZET(offset_value);
    size_t length;
    char *buffer;
    struct copy_args args;
    StringValue(string);
    length = (size_t)RSTRING_LEN(string);
    validate_range(data, offset, length);
    buffer = xmalloc(length ? length : 1);
    memcpy(buffer, RSTRING_PTR(string), length);
    args.destination = (char *)data->address + offset;
    args.source = buffer;
    args.length = length;
    data->busy++;
    rb_thread_call_without_gvl(without_gvl_memcpy, &args, RUBY_UBF_IO, NULL);
    data->busy--;
    xfree(buffer);
    return SIZET2NUM(length);
}

static VALUE
region_madvise(VALUE self, VALUE advice)
{
    region_data_t *data = get_region(self);
    if (advice != ID2SYM(rb_intern("dontneed")))
        rb_raise(rb_eArgError, "unknown advice");
    {
        int native_advice = MADV_DONTNEED;
#ifdef MADV_REMOVE
        if (data->shared) native_advice = MADV_REMOVE;
#endif
        if (madvise(data->address, data->size, native_advice) < 0)
            rb_syserr_fail(errno, "madvise");
    }
    return Qnil;
}

#ifdef __linux__
static uint64_t
register_mode(VALUE value)
{
    uint64_t result = 0;
    long i;
    VALUE modes = RB_TYPE_P(value, T_ARRAY) ? value : rb_ary_new_from_args(1, value);
    for (i = 0; i < RARRAY_LEN(modes); i++) {
        VALUE mode = rb_ary_entry(modes, i);
        if (mode == ID2SYM(rb_intern("missing"))) result |= UFFDIO_REGISTER_MODE_MISSING;
#ifdef UFFDIO_REGISTER_MODE_WP
        else if (mode == ID2SYM(rb_intern("wp"))) result |= UFFDIO_REGISTER_MODE_WP;
#endif
#ifdef UFFDIO_REGISTER_MODE_MINOR
        else if (mode == ID2SYM(rb_intern("minor"))) result |= UFFDIO_REGISTER_MODE_MINOR;
#endif
        else rb_raise(rb_eArgError, "unknown registration mode");
    }
    return result;
}
#endif

static VALUE
uffd_register(int argc, VALUE *argv, VALUE self)
{
    VALUE region_object, kwargs;
    VALUE values[1] = {Qundef};
    ID keys[] = {id_mode};
    uffd_data_t *data = get_uffd(self);
    region_data_t *region;
    rb_scan_args(argc, argv, "1:", &region_object, &kwargs);
    rb_get_kwargs(kwargs, keys, 1, 0, values);
    region = get_region(region_object);
#ifdef __linux__
    {
        struct uffdio_register request;
        memset(&request, 0, sizeof(request));
        request.range.start = (uint64_t)(uintptr_t)region->address;
        request.range.len = region->size;
        request.mode = register_mode(values[0]);
        if (ioctl(data->fd, UFFDIO_REGISTER, &request) < 0)
            rb_syserr_fail(errno, "UFFDIO_REGISTER");
        data->registered_start = (uintptr_t)region->address;
        data->registered_length = region->size;
        rb_ivar_set(self, rb_intern("@registered_region"), region_object);
        return ULL2NUM(request.ioctls);
    }
#else
    (void)data; (void)region; (void)values;
    raise_unsupported("userfaultfd is only available on Linux");
#endif
}

static VALUE
uffd_unregister(VALUE self, VALUE region_object)
{
    uffd_data_t *data = get_uffd(self);
    region_data_t *region = get_region(region_object);
#ifdef __linux__
    {
        struct uffdio_range range;
        range.start = (uint64_t)(uintptr_t)region->address;
        range.len = region->size;
        if (ioctl(data->fd, UFFDIO_UNREGISTER, &range) < 0)
            rb_syserr_fail(errno, "UFFDIO_UNREGISTER");
        if (data->registered_start == (uintptr_t)region->address) {
            data->registered_start = 0;
            data->registered_length = 0;
            rb_ivar_set(self, rb_intern("@registered_region"), Qnil);
        }
    }
#else
    (void)data; (void)region;
    raise_unsupported("userfaultfd is only available on Linux");
#endif
    return Qnil;
}

static VALUE
uffd_writeprotect(int argc, VALUE *argv, VALUE self)
{
    VALUE region_object, kwargs;
    VALUE values[1] = {Qundef};
    ID keys[] = {id_enabled};
    uffd_data_t *data = get_uffd(self);
    region_data_t *region;
    rb_scan_args(argc, argv, "1:", &region_object, &kwargs);
    rb_get_kwargs(kwargs, keys, 1, 0, values);
    region = get_region(region_object);
#if defined(__linux__) && defined(UFFDIO_WRITEPROTECT)
    {
        struct uffdio_writeprotect request;
        memset(&request, 0, sizeof(request));
        request.range.start = (uint64_t)(uintptr_t)region->address;
        request.range.len = region->size;
        request.mode = values[0] == Qtrue ? UFFDIO_WRITEPROTECT_MODE_WP : 0;
        if (ioctl(data->fd, UFFDIO_WRITEPROTECT, &request) < 0)
            rb_syserr_fail(errno, "UFFDIO_WRITEPROTECT");
    }
#else
    (void)data; (void)region; (void)values;
    raise_unsupported("write protection is unavailable on this platform");
#endif
    return Qnil;
}

#ifdef __linux__
struct read_args {
    int fd;
    void *buffer;
    size_t length;
    ssize_t result;
    int error;
};

static void *
without_gvl_read(void *ptr)
{
    struct read_args *args = ptr;
    args->result = read(args->fd, args->buffer, args->length);
    args->error = errno;
    return NULL;
}

struct ioctl_args {
    int fd;
    unsigned long request;
    void *argument;
    int result;
    int error;
};

static void *
without_gvl_ioctl(void *ptr)
{
    struct ioctl_args *args = ptr;
    args->result = ioctl(args->fd, args->request, args->argument);
    args->error = errno;
    return NULL;
}

static void
run_ioctl_without_gvl(int fd, unsigned long request, void *argument, const char *name)
{
    struct ioctl_args args = {fd, request, argument, 0, 0};
    rb_thread_call_without_gvl(without_gvl_ioctl, &args, RUBY_UBF_IO, NULL);
    if (args.result < 0) rb_syserr_fail(args.error, name);
}

static VALUE
new_event(VALUE klass)
{
    return rb_obj_alloc(klass);
}

static VALUE
new_fault(VALUE owner, const struct uffd_msg *message)
{
    VALUE event = new_event(cFault);
    VALUE flags = rb_ary_new();
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t address = (uintptr_t)message->arg.pagefault.address;
    address &= ~((uintptr_t)page_size - 1);
    if (message->arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE)
        rb_ary_push(flags, ID2SYM(rb_intern("write")));
#ifdef UFFD_PAGEFAULT_FLAG_WP
    if (message->arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP)
        rb_ary_push(flags, ID2SYM(rb_intern("wp")));
#endif
#ifdef UFFD_PAGEFAULT_FLAG_MINOR
    if (message->arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_MINOR)
        rb_ary_push(flags, ID2SYM(rb_intern("minor")));
#endif
    rb_ivar_set(event, rb_intern("@owner"), owner);
    rb_ivar_set(event, rb_intern("@address"), ULL2NUM(address));
    rb_ivar_set(event, rb_intern("@flags"), flags);
    rb_ivar_set(event, rb_intern("@page_size"), SIZET2NUM(page_size));
#ifdef UFFD_FEATURE_THREAD_ID
    rb_ivar_set(event, rb_intern("@thread_id"),
                UINT2NUM(message->arg.pagefault.feat.ptid));
#else
    rb_ivar_set(event, rb_intern("@thread_id"), Qnil);
#endif
    return event;
}

static VALUE
wrap_child_uffd(VALUE owner, int fd)
{
    uffd_data_t *parent = get_uffd(owner);
    uffd_data_t *child;
    VALUE object = uffd_alloc(cUserfaultFD);
    TypedData_Get_Struct(object, uffd_data_t, &uffd_type, child);
    child->fd = fd;
    child->pid = getpid();
    child->features = parent->features;
    child->enabled_features = parent->enabled_features;
    child->ioctls = parent->ioctls;
    child->backend = parent->backend;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return object;
}

static VALUE
parse_event(VALUE owner, const struct uffd_msg *message)
{
    VALUE event;
    switch (message->event) {
      case UFFD_EVENT_PAGEFAULT:
        return new_fault(owner, message);
#ifdef UFFD_EVENT_FORK
      case UFFD_EVENT_FORK:
        event = new_event(cForkEvent);
        rb_ivar_set(event, rb_intern("@child_uffd"),
                    wrap_child_uffd(owner, (int)message->arg.fork.ufd));
        return event;
#endif
#ifdef UFFD_EVENT_REMAP
      case UFFD_EVENT_REMAP:
        event = new_event(cRemapEvent);
        rb_ivar_set(event, rb_intern("@from"), ULL2NUM(message->arg.remap.from));
        rb_ivar_set(event, rb_intern("@to"), ULL2NUM(message->arg.remap.to));
        rb_ivar_set(event, rb_intern("@length"), ULL2NUM(message->arg.remap.len));
        return event;
#endif
#ifdef UFFD_EVENT_REMOVE
      case UFFD_EVENT_REMOVE:
        event = new_event(cRemoveEvent);
        rb_ivar_set(event, rb_intern("@start"), ULL2NUM(message->arg.remove.start));
        rb_ivar_set(event, rb_intern("@end"), ULL2NUM(message->arg.remove.end));
        return event;
#endif
#ifdef UFFD_EVENT_UNMAP
      case UFFD_EVENT_UNMAP:
        event = new_event(cUnmapEvent);
        rb_ivar_set(event, rb_intern("@start"), ULL2NUM(message->arg.remove.start));
        rb_ivar_set(event, rb_intern("@end"), ULL2NUM(message->arg.remove.end));
        return event;
#endif
      default:
        return Qnil;
    }
}
#endif

static VALUE
uffd_read_events(int argc, VALUE *argv, VALUE self)
{
    VALUE max_value = Qnil;
    uffd_data_t *data = get_uffd(self);
    size_t maximum = 16;
    rb_scan_args(argc, argv, "01", &max_value);
    if (!NIL_P(max_value)) maximum = NUM2SIZET(max_value);
    if (maximum == 0 || maximum > 1024)
        rb_raise(rb_eArgError, "max must be between 1 and 1024");
#ifdef __linux__
    {
        struct uffd_msg *messages = ALLOC_N(struct uffd_msg, maximum);
        struct read_args args = {
            data->fd, messages, sizeof(struct uffd_msg) * maximum, 0, 0
        };
        VALUE result = rb_ary_new();
        size_t count, i;
        rb_thread_call_without_gvl(without_gvl_read, &args, RUBY_UBF_IO, NULL);
        if (args.result < 0) {
            xfree(messages);
            if (args.error == EAGAIN || args.error == EINTR) return result;
            rb_syserr_fail(args.error, "read(userfaultfd)");
        }
        if ((size_t)args.result % sizeof(struct uffd_msg) != 0) {
            xfree(messages);
            rb_raise(eError, "short userfaultfd message");
        }
        count = (size_t)args.result / sizeof(struct uffd_msg);
        for (i = 0; i < count; i++) {
            VALUE event = parse_event(self, &messages[i]);
            if (!NIL_P(event)) rb_ary_push(result, event);
        }
        xfree(messages);
        return result;
    }
#else
    (void)data;
    raise_unsupported("userfaultfd is only available on Linux");
#endif
}

static VALUE
uffd_parse_message(VALUE klass, VALUE string)
{
    (void)klass;
    StringValue(string);
#ifdef __linux__
    if (RSTRING_LEN(string) != (long)sizeof(struct uffd_msg))
        rb_raise(rb_eArgError, "message has the wrong size");
    return parse_event(Qnil, (const struct uffd_msg *)RSTRING_PTR(string));
#else
    raise_unsupported("userfaultfd messages are only available on Linux");
#endif
}

static uffd_data_t *
fault_owner(VALUE self)
{
    return get_uffd(rb_ivar_get(self, rb_intern("@owner")));
}

#ifdef __linux__
static uintptr_t
fault_address(VALUE self)
{
    return (uintptr_t)NUM2ULL(rb_ivar_get(self, rb_intern("@address")));
}
#endif

static size_t
fault_page_size(VALUE self)
{
    return NUM2SIZET(rb_ivar_get(self, rb_intern("@page_size")));
}

static int
wake_requested(VALUE kwargs)
{
    VALUE values[1] = {Qundef};
    ID keys[] = {id_wake};
    if (NIL_P(kwargs)) return 1;
    rb_get_kwargs(kwargs, keys, 0, 1, values);
    return values[0] == Qundef || values[0] != Qfalse;
}

static VALUE
fault_copy(int argc, VALUE *argv, VALUE self)
{
    VALUE string, kwargs;
    uffd_data_t *owner = fault_owner(self);
    size_t length, page_size = fault_page_size(self);
    char *buffer;
    int wake;
    rb_scan_args(argc, argv, "1:", &string, &kwargs);
    StringValue(string);
    length = (size_t)RSTRING_LEN(string);
    if (length == 0 || length % page_size != 0)
        rb_raise(rb_eArgError, "copy length must be a positive multiple of page_size");
    wake = wake_requested(kwargs);
#ifdef __linux__
    {
        struct uffdio_copy request;
        buffer = xmalloc(length);
        memcpy(buffer, RSTRING_PTR(string), length);
        memset(&request, 0, sizeof(request));
        request.dst = (uint64_t)fault_address(self);
        request.src = (uint64_t)(uintptr_t)buffer;
        request.len = length;
        request.mode = wake ? 0 : UFFDIO_COPY_MODE_DONTWAKE;
        {
            struct ioctl_args args = {
                owner->fd, UFFDIO_COPY, &request, 0, 0
            };
            rb_thread_call_without_gvl(without_gvl_ioctl, &args, RUBY_UBF_IO, NULL);
            if (args.result < 0) {
                int saved_errno = args.error;
                xfree(buffer);
                rb_syserr_fail(saved_errno, "UFFDIO_COPY");
            }
        }
        xfree(buffer);
        if (request.copy < 0) rb_syserr_fail((int)-request.copy, "UFFDIO_COPY");
        return LL2NUM(request.copy);
    }
#else
    (void)owner; (void)buffer; (void)wake;
    raise_unsupported("userfaultfd is only available on Linux");
#endif
}

static VALUE
fault_zero(int argc, VALUE *argv, VALUE self)
{
    VALUE kwargs;
    uffd_data_t *owner = fault_owner(self);
    int wake;
    rb_scan_args(argc, argv, "0:", &kwargs);
    wake = wake_requested(kwargs);
#ifdef __linux__
    {
        struct uffdio_zeropage request;
        memset(&request, 0, sizeof(request));
        request.range.start = (uint64_t)fault_address(self);
        request.range.len = fault_page_size(self);
        request.mode = wake ? 0 : UFFDIO_ZEROPAGE_MODE_DONTWAKE;
        run_ioctl_without_gvl(owner->fd, UFFDIO_ZEROPAGE, &request, "UFFDIO_ZEROPAGE");
        if (request.zeropage < 0)
            rb_syserr_fail((int)-request.zeropage, "UFFDIO_ZEROPAGE");
        return LL2NUM(request.zeropage);
    }
#else
    (void)owner; (void)wake;
    raise_unsupported("userfaultfd is only available on Linux");
#endif
}

static VALUE
fault_wake(VALUE self)
{
    uffd_data_t *owner = fault_owner(self);
#ifdef __linux__
    {
        struct uffdio_range range;
        range.start = (uint64_t)fault_address(self);
        range.len = fault_page_size(self);
        run_ioctl_without_gvl(owner->fd, UFFDIO_WAKE, &range, "UFFDIO_WAKE");
    }
#else
    (void)owner;
    raise_unsupported("userfaultfd is only available on Linux");
#endif
    return Qnil;
}

static VALUE
fault_continue(int argc, VALUE *argv, VALUE self)
{
    VALUE kwargs;
    uffd_data_t *owner = fault_owner(self);
    int wake;
    rb_scan_args(argc, argv, "0:", &kwargs);
    wake = wake_requested(kwargs);
#if defined(__linux__) && defined(UFFDIO_CONTINUE)
    {
        struct uffdio_continue request;
        memset(&request, 0, sizeof(request));
        request.range.start = (uint64_t)fault_address(self);
        request.range.len = fault_page_size(self);
        request.mode = wake ? 0 : UFFDIO_CONTINUE_MODE_DONTWAKE;
        run_ioctl_without_gvl(owner->fd, UFFDIO_CONTINUE, &request, "UFFDIO_CONTINUE");
        if (request.mapped < 0)
            rb_syserr_fail((int)-request.mapped, "UFFDIO_CONTINUE");
        return LL2NUM(request.mapped);
    }
#else
    (void)owner; (void)wake;
    raise_unsupported("UFFDIO_CONTINUE is unavailable on this platform");
#endif
}

static VALUE
fault_poison(int argc, VALUE *argv, VALUE self)
{
    VALUE kwargs;
    uffd_data_t *owner = fault_owner(self);
    int wake;
    rb_scan_args(argc, argv, "0:", &kwargs);
    wake = wake_requested(kwargs);
#if defined(__linux__) && defined(UFFDIO_POISON)
    {
        struct uffdio_poison request;
        memset(&request, 0, sizeof(request));
        request.range.start = (uint64_t)fault_address(self);
        request.range.len = fault_page_size(self);
        request.mode = wake ? 0 : UFFDIO_POISON_MODE_DONTWAKE;
        run_ioctl_without_gvl(owner->fd, UFFDIO_POISON, &request, "UFFDIO_POISON");
        if (request.updated < 0)
            rb_syserr_fail((int)-request.updated, "UFFDIO_POISON");
        return LL2NUM(request.updated);
    }
#else
    (void)owner; (void)wake;
    raise_unsupported("UFFDIO_POISON is unavailable on this platform");
#endif
}

static VALUE
fault_move(int argc, VALUE *argv, VALUE self)
{
    VALUE source_object, kwargs;
    VALUE values[2] = {Qundef, Qundef};
    ID keys[] = {id_offset, id_wake};
    region_data_t *source;
    size_t offset = 0, page_size = fault_page_size(self);
    int wake = 1;
    uffd_data_t *owner;
    rb_scan_args(argc, argv, "1:", &source_object, &kwargs);
    source = get_region(source_object);
    if (!NIL_P(kwargs)) {
        rb_get_kwargs(kwargs, keys, 0, 2, values);
        if (values[0] != Qundef) offset = NUM2SIZET(values[0]);
        if (values[1] != Qundef) wake = values[1] != Qfalse;
    }
    if (offset % page_size != 0 || offset > source->size ||
        page_size > source->size - offset)
        rb_raise(rb_eArgError, "source offset must select a complete aligned page");
    owner = fault_owner(self);
#if defined(__linux__) && defined(UFFDIO_MOVE)
    {
        struct uffdio_move request;
        memset(&request, 0, sizeof(request));
        request.dst = (uint64_t)fault_address(self);
        request.src = (uint64_t)(uintptr_t)((char *)source->address + offset);
        request.len = page_size;
        request.mode = wake ? 0 : UFFDIO_MOVE_MODE_DONTWAKE;
        run_ioctl_without_gvl(owner->fd, UFFDIO_MOVE, &request, "UFFDIO_MOVE");
        if (request.move < 0) rb_syserr_fail((int)-request.move, "UFFDIO_MOVE");
        return LL2NUM(request.move);
    }
#else
    (void)owner; (void)wake;
    raise_unsupported("UFFDIO_MOVE is unavailable on this platform");
#endif
}

#ifdef __linux__
static int
fill_from_file(native_handler_data_t *data, void *buffer, off_t offset)
{
    size_t done = 0;
    memset(buffer, 0, data->page_size);
    while (done < data->page_size) {
        ssize_t count = pread(data->source_fd, (char *)buffer + done,
                              data->page_size - done, offset + (off_t)done);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}

static int
native_resolve_fault(native_handler_data_t *data, const struct uffd_msg *message,
                     void *buffer)
{
    uintptr_t address = (uintptr_t)message->arg.pagefault.address;
    uintptr_t offset;
    address &= ~((uintptr_t)data->page_size - 1);
    if (address < data->start || address >= data->start + data->length) {
        errno = EFAULT;
        return -1;
    }
    offset = address - data->start;
    if (data->mode == 1) {
        struct uffdio_zeropage request;
        memset(&request, 0, sizeof(request));
        request.range.start = address;
        request.range.len = data->page_size;
        return ioctl(data->uffd_fd, UFFDIO_ZEROPAGE, &request);
    }

    if (data->mode == 2 && fill_from_file(data, buffer, (off_t)offset) < 0)
        return -1;
    if (data->mode == 3)
        memcpy(buffer, (const char *)data->source_address + offset, data->page_size);

    {
        struct uffdio_copy request;
        memset(&request, 0, sizeof(request));
        request.dst = address;
        request.src = (uint64_t)(uintptr_t)buffer;
        request.len = data->page_size;
        return ioctl(data->uffd_fd, UFFDIO_COPY, &request);
    }
}

static void *
native_handler_main(void *ptr)
{
    native_handler_data_t *data = ptr;
    struct pollfd fds[2];
    struct uffd_msg messages[16];
    void *buffer = NULL;
    if (data->mode != 1) {
        buffer = malloc(data->page_size);
        if (!buffer) {
            data->error = ENOMEM;
            data->running = 0;
            return NULL;
        }
    }
    fds[0].fd = data->uffd_fd;
    fds[0].events = POLLIN;
    fds[1].fd = data->stop_pipe[0];
    fds[1].events = POLLIN;
    while (1) {
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            data->error = errno;
            break;
        }
        if (fds[1].revents) break;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            data->error = EIO;
            break;
        }
        if (fds[0].revents & POLLIN) {
            ssize_t count = read(data->uffd_fd, messages, sizeof(messages));
            size_t i, length;
            if (count < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                data->error = errno;
                break;
            }
            if ((size_t)count % sizeof(struct uffd_msg) != 0) {
                data->error = EIO;
                break;
            }
            length = (size_t)count / sizeof(struct uffd_msg);
            for (i = 0; i < length; i++) {
                if (messages[i].event == UFFD_EVENT_PAGEFAULT) {
                    if (native_resolve_fault(data, &messages[i], buffer) < 0) {
                        data->error = errno;
                        goto done;
                    }
                }
#ifdef UFFD_EVENT_FORK
                else if (messages[i].event == UFFD_EVENT_FORK) {
                    close((int)messages[i].arg.fork.ufd);
                }
#endif
            }
        }
    }
done:
    free(buffer);
    data->running = 0;
    return NULL;
}
#endif

struct join_args {
    pthread_t thread;
    int result;
};

static void
signal_stop_pipe(int fd)
{
    char byte = 0;
    ssize_t result;
    if (fd < 0) return;
    do {
        result = write(fd, &byte, 1);
    } while (result < 0 && errno == EINTR);
}

static void *
without_gvl_join(void *ptr)
{
    struct join_args *args = ptr;
    args->result = pthread_join(args->thread, NULL);
    return NULL;
}

static void
native_handler_stop_and_join(native_handler_data_t *data)
{
    struct join_args args;
    if (data->pid != getpid()) {
        data->joined = 1;
        data->running = 0;
        return;
    }
    if (data->joined || !data->started) return;
    signal_stop_pipe(data->stop_pipe[1]);
    args.thread = data->thread;
    args.result = 0;
    if (ruby_native_thread_p())
        rb_thread_call_without_gvl(without_gvl_join, &args, RUBY_UBF_IO, NULL);
    else
        args.result = pthread_join(args.thread, NULL);
    data->joined = 1;
    data->running = 0;
}

static VALUE
uffd_start_native_handler(VALUE self, VALUE mode, VALUE source)
{
    uffd_data_t *owner = get_uffd(self);
    native_handler_data_t *data;
    VALUE object;
    if (!owner->registered_start || !owner->registered_length)
        rb_raise(eError, "register a region before starting a native handler");
    object = native_handler_alloc(cNativeHandler);
    TypedData_Get_Struct(object, native_handler_data_t, &native_handler_type, data);
    data->owner = self;
    data->source = source;
    data->uffd_fd = owner->fd;
    data->start = owner->registered_start;
    data->length = owner->registered_length;
    data->page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (mode == ID2SYM(rb_intern("zero_fill"))) {
        data->mode = 1;
    } else if (mode == ID2SYM(rb_intern("backing_file"))) {
        VALUE fd;
        if (NIL_P(source)) rb_raise(rb_eArgError, "io is required for backing_file");
        fd = rb_funcall(source, rb_intern("fileno"), 0);
        data->source_fd = dup(NUM2INT(fd));
        if (data->source_fd < 0) rb_syserr_fail(errno, "dup");
        data->mode = 2;
    } else if (mode == ID2SYM(rb_intern("prefilled"))) {
        region_data_t *region;
        if (NIL_P(source)) rb_raise(rb_eArgError, "source is required for prefilled");
        region = get_region(source);
        if (region->size < data->length)
            rb_raise(rb_eArgError, "source region is smaller than registered region");
        data->source_address = region->address;
        data->mode = 3;
    } else {
        rb_raise(rb_eArgError, "unknown native handler mode");
    }
#ifdef __linux__
    if (pipe(data->stop_pipe) < 0) rb_syserr_fail(errno, "pipe");
    fcntl(data->stop_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(data->stop_pipe[1], F_SETFD, FD_CLOEXEC);
    data->running = 1;
    if (pthread_create(&data->thread, NULL, native_handler_main, data) != 0) {
        data->running = 0;
        rb_raise(eError, "failed to start native handler thread");
    }
    data->started = 1;
#else
    raise_unsupported("native handlers are only available on Linux");
#endif
    return object;
}

static VALUE
native_handler_stop(VALUE self)
{
    native_handler_data_t *data;
    TypedData_Get_Struct(self, native_handler_data_t, &native_handler_type, data);
    if (data->pid != getpid()) rb_raise(eError, "handler cannot be used after fork");
    native_handler_stop_and_join(data);
    if (data->error) rb_syserr_fail(data->error, "userfaultfd handler");
    return self;
}

static VALUE
native_handler_running_p(VALUE self)
{
    native_handler_data_t *data;
    TypedData_Get_Struct(self, native_handler_data_t, &native_handler_type, data);
    return data->running ? Qtrue : Qfalse;
}

#ifdef __linux__
struct event_record {
    int source_fd;
    struct uffd_msg message;
};

static void
event_reader_remove_fd(event_reader_data_t *data, size_t index)
{
    size_t i;
    if (data->owned[index]) close(data->fds[index]);
    for (i = index + 1; i < data->fd_count; i++) {
        data->fds[i - 1] = data->fds[i];
        data->owned[i - 1] = data->owned[i];
    }
    data->fd_count--;
}

static int
event_reader_emit(event_reader_data_t *data, int source_fd,
                  const struct uffd_msg *message)
{
    struct event_record record;
    ssize_t written;
    record.source_fd = source_fd;
    record.message = *message;
    do {
        written = write(data->output_pipe[1], &record, sizeof(record));
    } while (written < 0 && errno == EINTR);
    if (written == (ssize_t)sizeof(record)) return 0;
    data->error = written < 0 && errno != EAGAIN ? errno : ENOBUFS;
    return -1;
}

static void *
event_reader_main(void *ptr)
{
    event_reader_data_t *data = ptr;
    /* ponytail: 64 monitored processes; use epoll if larger fan-out is needed. */
    struct pollfd poll_fds[EVENT_READER_MAX_FDS + 1];
    while (1) {
        size_t i, polled_count = data->fd_count;
        int ready;
        for (i = 0; i < polled_count; i++) {
            poll_fds[i].fd = data->fds[i];
            poll_fds[i].events = POLLIN;
            poll_fds[i].revents = 0;
        }
        poll_fds[polled_count].fd = data->stop_pipe[0];
        poll_fds[polled_count].events = POLLIN;
        poll_fds[polled_count].revents = 0;
        ready = poll(poll_fds, polled_count + 1, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            data->error = errno;
            break;
        }
        if (poll_fds[polled_count].revents) break;
        for (i = 0; i < polled_count; i++) {
            if (poll_fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                if (i == 0) {
                    data->error = EIO;
                    goto done;
                }
                event_reader_remove_fd(data, i);
                goto repoll;
            }
            if (poll_fds[i].revents & POLLIN) {
                struct uffd_msg messages[16];
                ssize_t count = read(data->fds[i], messages, sizeof(messages));
                size_t j, length;
                if (count < 0) {
                    if (errno == EAGAIN || errno == EINTR) {
                        continue;
                    }
                    data->error = errno;
                    goto done;
                }
                if ((size_t)count % sizeof(struct uffd_msg) != 0) {
                    data->error = EIO;
                    goto done;
                }
                length = (size_t)count / sizeof(struct uffd_msg);
                for (j = 0; j < length; j++) {
#ifdef UFFD_EVENT_FORK
                    if (messages[j].event == UFFD_EVENT_FORK) {
                        int child_fd = (int)messages[j].arg.fork.ufd;
                        if (data->fd_count == EVENT_READER_MAX_FDS) {
                            close(child_fd);
                            data->error = EMFILE;
                            goto done;
                        }
                        fcntl(child_fd, F_SETFD, FD_CLOEXEC);
                        fcntl(child_fd, F_SETFL,
                              fcntl(child_fd, F_GETFL) | O_NONBLOCK);
                        data->fds[data->fd_count] = child_fd;
                        data->owned[data->fd_count] = 1;
                        data->fd_count++;
                    }
#endif
                    if (event_reader_emit(data, data->fds[i], &messages[j]) < 0)
                        goto done;
                }
            }
        }
repoll:
        ;
    }
done:
    data->running = 0;
    close(data->output_pipe[1]);
    data->output_pipe[1] = -1;
    return NULL;
}
#endif

static void
event_reader_stop_and_join(event_reader_data_t *data)
{
    struct join_args args;
    if (data->pid != getpid()) {
        data->joined = 1;
        data->running = 0;
        return;
    }
    if (data->joined || !data->started) return;
    signal_stop_pipe(data->stop_pipe[1]);
    args.thread = data->thread;
    args.result = 0;
    if (ruby_native_thread_p())
        rb_thread_call_without_gvl(without_gvl_join, &args, RUBY_UBF_IO, NULL);
    else
        args.result = pthread_join(args.thread, NULL);
    data->joined = 1;
    data->running = 0;
}

static VALUE
uffd_start_event_reader(VALUE self)
{
    uffd_data_t *owner = get_uffd(self);
    event_reader_data_t *data;
    VALUE object = event_reader_alloc(cEventReader);
    TypedData_Get_Struct(object, event_reader_data_t, &event_reader_type, data);
    data->owner = self;
    data->fds[0] = owner->fd;
    data->owned[0] = 0;
    data->fd_count = 1;
    rb_ivar_set(object, rb_intern("@owners"), rb_hash_new());
    rb_hash_aset(rb_ivar_get(object, rb_intern("@owners")),
                 INT2NUM(owner->fd), self);
#ifdef __linux__
    if (pipe(data->stop_pipe) < 0 || pipe(data->output_pipe) < 0)
        rb_syserr_fail(errno, "pipe");
    fcntl(data->stop_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(data->stop_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(data->output_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(data->output_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(data->output_pipe[0], F_SETFL,
          fcntl(data->output_pipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(data->output_pipe[1], F_SETFL,
          fcntl(data->output_pipe[1], F_GETFL) | O_NONBLOCK);
    data->running = 1;
    if (pthread_create(&data->thread, NULL, event_reader_main, data) != 0) {
        data->running = 0;
        rb_raise(eError, "failed to start event reader thread");
    }
    data->started = 1;
#else
    raise_unsupported("event readers are only available on Linux");
#endif
    return object;
}

static VALUE
event_reader_fileno(VALUE self)
{
    event_reader_data_t *data;
    TypedData_Get_Struct(self, event_reader_data_t, &event_reader_type, data);
    if (data->output_pipe[0] < 0) rb_raise(eError, "closed event reader");
    return INT2NUM(data->output_pipe[0]);
}

static VALUE
event_reader_read_events(int argc, VALUE *argv, VALUE self)
{
    VALUE max_value = Qnil;
    event_reader_data_t *data;
    size_t maximum = 16;
    rb_scan_args(argc, argv, "01", &max_value);
    if (!NIL_P(max_value)) maximum = NUM2SIZET(max_value);
    if (maximum == 0 || maximum > 1024)
        rb_raise(rb_eArgError, "max must be between 1 and 1024");
    TypedData_Get_Struct(self, event_reader_data_t, &event_reader_type, data);
#ifdef __linux__
    {
        struct event_record *records = ALLOC_N(struct event_record, maximum);
        ssize_t count = read(data->output_pipe[0], records,
                             sizeof(struct event_record) * maximum);
        VALUE result = rb_ary_new();
        VALUE owners = rb_ivar_get(self, rb_intern("@owners"));
        size_t length, i;
        if (count < 0) {
            xfree(records);
            if (errno == EAGAIN || errno == EINTR) return result;
            rb_syserr_fail(errno, "read(userfaultfd event pipe)");
        }
        if (count == 0) {
            xfree(records);
            if (data->error) rb_syserr_fail(data->error, "userfaultfd event reader");
            return result;
        }
        if ((size_t)count % sizeof(struct event_record) != 0) {
            xfree(records);
            rb_raise(eError, "short event-reader record");
        }
        length = (size_t)count / sizeof(struct event_record);
        for (i = 0; i < length; i++) {
            VALUE owner = rb_hash_aref(owners, INT2NUM(records[i].source_fd));
            VALUE event;
            if (NIL_P(owner)) {
                xfree(records);
                rb_raise(eError, "unknown userfaultfd event source");
            }
#ifdef UFFD_EVENT_FORK
            if (records[i].message.event == UFFD_EVENT_FORK) {
                int raw_fd = (int)records[i].message.arg.fork.ufd;
                int duplicated = dup(raw_fd);
                if (duplicated < 0) {
                    xfree(records);
                    rb_syserr_fail(errno, "dup(child userfaultfd)");
                }
                records[i].message.arg.fork.ufd = (uint32_t)duplicated;
                event = parse_event(owner, &records[i].message);
                rb_hash_aset(owners, INT2NUM(raw_fd),
                             rb_ivar_get(event, rb_intern("@child_uffd")));
            } else
#endif
            {
                event = parse_event(owner, &records[i].message);
            }
            if (!NIL_P(event)) rb_ary_push(result, event);
        }
        xfree(records);
        return result;
    }
#else
    raise_unsupported("event readers are only available on Linux");
#endif
}

static VALUE
event_reader_stop(VALUE self)
{
    event_reader_data_t *data;
    TypedData_Get_Struct(self, event_reader_data_t, &event_reader_type, data);
    if (data->pid != getpid()) rb_raise(eError, "event reader cannot be used after fork");
    event_reader_stop_and_join(data);
    event_reader_close_owned(data);
    if (data->error) rb_syserr_fail(data->error, "userfaultfd event reader");
    return self;
}

static VALUE
event_reader_running_p(VALUE self)
{
    event_reader_data_t *data;
    TypedData_Get_Struct(self, event_reader_data_t, &event_reader_type, data);
    return data->running ? Qtrue : Qfalse;
}

void
Init_userfaultfd(void)
{
    cUserfaultFD = rb_define_class("UserfaultFD", rb_cObject);
    eError = rb_const_get(cUserfaultFD, rb_intern("Error"));
    eUnsupportedError = rb_const_get(cUserfaultFD, rb_intern("UnsupportedError"));
    cRegion = rb_define_class_under(cUserfaultFD, "Region", rb_cObject);

    id_size = rb_intern("size");
    id_shared = rb_intern("shared");
    id_huge = rb_intern("huge");
    id_features = rb_intern("features");
    id_mode = rb_intern("mode");
    id_enabled = rb_intern("enabled");
    id_wake = rb_intern("wake");
    id_offset = rb_intern("offset");

    rb_define_alloc_func(cUserfaultFD, uffd_alloc);
    rb_define_method(cUserfaultFD, "initialize", uffd_initialize, -1);
    rb_define_singleton_method(cUserfaultFD, "supported?", uffd_supported, 0);
    rb_define_singleton_method(cUserfaultFD, "features", uffd_features, 0);
    rb_define_private_method(rb_singleton_class(cUserfaultFD), "parse_message",
                             uffd_parse_message, 1);
    rb_define_method(cUserfaultFD, "features", uffd_instance_features, 0);
    rb_define_method(cUserfaultFD, "enabled_features", uffd_enabled_features, 0);
    rb_define_method(cUserfaultFD, "backend", uffd_backend, 0);
    rb_define_method(cUserfaultFD, "fileno", uffd_fileno, 0);
    rb_define_method(cUserfaultFD, "close", uffd_close, 0);
    rb_define_method(cUserfaultFD, "closed?", uffd_closed_p, 0);
    rb_define_method(cUserfaultFD, "register", uffd_register, -1);
    rb_define_method(cUserfaultFD, "unregister", uffd_unregister, 1);
    rb_define_method(cUserfaultFD, "writeprotect", uffd_writeprotect, -1);
    rb_define_method(cUserfaultFD, "read_events", uffd_read_events, -1);
    rb_define_private_method(cUserfaultFD, "start_native_handler",
                             uffd_start_native_handler, 2);
    rb_define_private_method(cUserfaultFD, "start_event_reader",
                             uffd_start_event_reader, 0);

    rb_define_alloc_func(cRegion, region_alloc);
    rb_define_method(cRegion, "initialize", region_initialize, -1);
    rb_define_method(cRegion, "address", region_address, 0);
    rb_define_method(cRegion, "to_ptr", region_address, 0);
    rb_define_method(cRegion, "size", region_size, 0);
    rb_define_method(cRegion, "page_size", region_page_size, 0);
    rb_define_method(cRegion, "read", region_read, 2);
    rb_define_method(cRegion, "write", region_write, 2);
    rb_define_method(cRegion, "madvise", region_madvise, 1);
    rb_define_method(cRegion, "unmap", region_unmap, 0);
    rb_define_method(cRegion, "unmapped?", region_unmapped_p, 0);

    cFault = rb_define_class_under(cUserfaultFD, "Fault", rb_cObject);
    rb_define_attr(cFault, "address", 1, 0);
    rb_define_attr(cFault, "flags", 1, 0);
    rb_define_attr(cFault, "thread_id", 1, 0);
    rb_define_method(cFault, "copy", fault_copy, -1);
    rb_define_method(cFault, "zero", fault_zero, -1);
    rb_define_method(cFault, "continue", fault_continue, -1);
    rb_define_method(cFault, "poison", fault_poison, -1);
    rb_define_method(cFault, "move", fault_move, -1);
    rb_define_method(cFault, "wake", fault_wake, 0);

    cForkEvent = rb_define_class_under(cUserfaultFD, "ForkEvent", rb_cObject);
    rb_define_attr(cForkEvent, "child_uffd", 1, 0);
    cRemapEvent = rb_define_class_under(cUserfaultFD, "RemapEvent", rb_cObject);
    rb_define_attr(cRemapEvent, "from", 1, 0);
    rb_define_attr(cRemapEvent, "to", 1, 0);
    rb_define_attr(cRemapEvent, "length", 1, 0);
    cRemoveEvent = rb_define_class_under(cUserfaultFD, "RemoveEvent", rb_cObject);
    rb_define_attr(cRemoveEvent, "start", 1, 0);
    rb_define_attr(cRemoveEvent, "end", 1, 0);
    cUnmapEvent = rb_define_class_under(cUserfaultFD, "UnmapEvent", rb_cObject);
    rb_define_attr(cUnmapEvent, "start", 1, 0);
    rb_define_attr(cUnmapEvent, "end", 1, 0);

    cNativeHandler = rb_define_class_under(cUserfaultFD, "NativeHandler", rb_cObject);
    rb_define_alloc_func(cNativeHandler, native_handler_alloc);
    rb_define_method(cNativeHandler, "stop", native_handler_stop, 0);
    rb_define_method(cNativeHandler, "running?", native_handler_running_p, 0);

    cEventReader = rb_define_class_under(cUserfaultFD, "EventReader", rb_cObject);
    rb_define_alloc_func(cEventReader, event_reader_alloc);
    rb_define_method(cEventReader, "fileno", event_reader_fileno, 0);
    rb_define_method(cEventReader, "read_events", event_reader_read_events, -1);
    rb_define_method(cEventReader, "stop", event_reader_stop, 0);
    rb_define_method(cEventReader, "running?", event_reader_running_p, 0);

    userfaultfd_define_constants(cUserfaultFD);
}
