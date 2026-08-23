#include "ruby.h"
#include "ruby/thread.h"
#include "extconf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
} region_data_t;

static VALUE cUserfaultFD;
static VALUE cRegion;
static VALUE eError;
static VALUE eUnsupportedError;

static ID id_size;
static ID id_shared;
static ID id_huge;
static ID id_features;
static ID id_mode;
static ID id_enabled;

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
    uffd_data_t *data;

    rb_scan_args(argc, argv, "0:", &kwargs);
    if (!NIL_P(kwargs)) {
        ID keys[] = {id_features};
        rb_get_kwargs(kwargs, keys, 0, 1, values);
        if (values[0] != Qundef) {
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
        if ((requested & available) != requested)
            raise_unsupported("requested userfaultfd features are unavailable");

        data->fd = open_uffd(&data->backend);
        if (data->fd < 0) rb_syserr_fail(errno, "userfaultfd");
        if (query_api(data->fd, requested, &data->features, &data->ioctls) < 0) {
            saved_errno = errno;
            close(data->fd);
            data->fd = -1;
            if (saved_errno == EINVAL)
                raise_unsupported("requested userfaultfd features were rejected");
            rb_syserr_fail(saved_errno, "UFFDIO_API");
        }
    }
#else
    (void)requested;
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
    if (madvise(data->address, data->size, MADV_DONTNEED) < 0)
        rb_syserr_fail(errno, "madvise");
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

    rb_define_alloc_func(cUserfaultFD, uffd_alloc);
    rb_define_method(cUserfaultFD, "initialize", uffd_initialize, -1);
    rb_define_singleton_method(cUserfaultFD, "supported?", uffd_supported, 0);
    rb_define_singleton_method(cUserfaultFD, "features", uffd_features, 0);
    rb_define_method(cUserfaultFD, "features", uffd_instance_features, 0);
    rb_define_method(cUserfaultFD, "backend", uffd_backend, 0);
    rb_define_method(cUserfaultFD, "fileno", uffd_fileno, 0);
    rb_define_method(cUserfaultFD, "close", uffd_close, 0);
    rb_define_method(cUserfaultFD, "closed?", uffd_closed_p, 0);
    rb_define_method(cUserfaultFD, "register", uffd_register, -1);
    rb_define_method(cUserfaultFD, "unregister", uffd_unregister, 1);
    rb_define_method(cUserfaultFD, "writeprotect", uffd_writeprotect, -1);

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
}
