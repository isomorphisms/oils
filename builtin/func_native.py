from __future__ import print_function

from _devbuild.gen.syntax_asdl import loc_t
from _devbuild.gen.value_asdl import value, value_e, value_t, Obj
from core import error
from core import vm
from frontend import typed_args
from mycpp import mops

import libc

from typing import Dict, List, Optional, cast


PROT_READ = 1
PROT_WRITE = 2
PROT_EXECUTE = 4

MAP_PRIVATE = 1
MAP_SHARED = 2
MAP_ANONYMOUS = 4

SYNC_SYNC = 1
SYNC_ASYNC = 2
SYNC_INVALIDATE = 4

OPEN_READ_ONLY = 1
OPEN_WRITE_ONLY = 2
OPEN_READ_WRITE = 4
OPEN_CREATE = 8
OPEN_EXCLUSIVE = 16
OPEN_TRUNCATE = 32
OPEN_APPEND = 64
OPEN_DIRECTORY = 128
OPEN_NO_FOLLOW = 256
OPEN_CLOSE_ON_EXEC = 512


def _Int(i):
    # type: (int) -> value.Int
    return value.Int(mops.IntWiden(i))


def _Props():
    # type: () -> Dict[str, value_t]
    props = {}  # type: Dict[str, value_t]
    return props


def _NativeObject(kind):
    # type: (str) -> Obj
    props = _Props()
    props['native kind'] = value.Str(kind)
    return Obj(None, props)


def _Error(errno_num):
    # type: (int) -> Obj
    result = _NativeObject('NativeError')
    result.d['number'] = _Int(errno_num)
    result.d['name'] = value.Str(libc.grease_errno_name(errno_num))
    result.d['message'] = value.Str(libc.grease_errno_message(errno_num))
    return result


def _Ok(result):
    # type: (value_t) -> Obj
    props = _Props()
    props['ok'] = value.Bool(True)
    props['value'] = result
    props['error'] = value.Null
    return Obj(None, props)


def _Err(errno_num):
    # type: (int) -> Obj
    props = _Props()
    props['ok'] = value.Bool(False)
    props['value'] = value.Null
    props['error'] = _Error(errno_num)
    return Obj(None, props)


def _Status(errno_num):
    # type: (int) -> Obj
    if errno_num == 0:
        return _Ok(value.Null)
    return _Err(errno_num)


def _Address(token):
    # type: (int) -> Obj
    address = _NativeObject('Address')
    address.d['token'] = _Int(token)
    address.d['valid'] = value.Bool(True)
    return address


def _Mapping(token, length):
    # type: (int, mops.BigInt) -> Obj
    mapping = _NativeObject('Mapping')
    mapping.d['address'] = _Address(token)
    mapping.d['length'] = value.Int(length)
    mapping.d['active'] = value.Bool(True)
    return mapping


def _FileDescriptor(token, directory, borrowed=False):
    # type: (int, bool, bool) -> Obj
    descriptor = _NativeObject('FileDescriptor')
    descriptor.d['token'] = _Int(token)
    descriptor.d['directory'] = value.Bool(directory)
    descriptor.d['borrowed'] = value.Bool(borrowed)
    descriptor.d['closed'] = value.Bool(False)
    return descriptor


def _Property(obj, name, blame):
    # type: (Obj, str, loc_t) -> value_t
    result = obj.d.get(name)
    if result is None:
        raise error.TypeErr(obj, 'Expected native %s property' % name, blame)
    return result


def _Kind(obj, blame):
    # type: (Obj, loc_t) -> str
    kind = _Property(obj, 'native kind', blame)
    if kind.tag() != value_e.Str:
        raise error.TypeErr(kind, 'Expected native object kind', blame)
    return cast(value.Str, kind).s


def _BoolProperty(obj, name, blame):
    # type: (Obj, str, loc_t) -> bool
    result = _Property(obj, name, blame)
    if result.tag() != value_e.Bool:
        raise error.TypeErr(result, 'Expected Bool property %s' % name, blame)
    return cast(value.Bool, result).b


def _Token(obj, kind, blame):
    # type: (Obj, str, loc_t) -> int
    if _Kind(obj, blame) != kind:
        raise error.TypeErr(obj, 'Expected native %s' % kind, blame)
    token = _Property(obj, 'token', blame)
    if token.tag() != value_e.Int:
        raise error.TypeErr(token, 'Expected native handle token', blame)
    big = cast(value.Int, token).i
    if mops.Greater(mops.ZERO, big):
        raise error.TypeErr(token, 'Native handle token must be non-negative', blame)
    max_token = mops.IntWiden(2147483647)
    if mops.Greater(big, max_token):
        raise error.TypeErr(token, 'Native handle token is out of range', blame)
    return mops.BigTruncate(big)


def _DescriptorToken(obj, blame):
    # type: (Obj, loc_t) -> int
    if _BoolProperty(obj, 'closed', blame):
        raise error.TypeErr(obj, 'FileDescriptor is closed', blame)
    return _Token(obj, 'FileDescriptor', blame)


def _MappingToken(mapping, blame):
    # type: (Obj, loc_t) -> int
    if _Kind(mapping, blame) != 'Mapping':
        raise error.TypeErr(mapping, 'Expected native Mapping', blame)
    if not _BoolProperty(mapping, 'active', blame):
        raise error.TypeErr(mapping, 'Mapping is not active', blame)
    address = _Property(mapping, 'address', blame)
    if address.tag() != value_e.Obj:
        raise error.TypeErr(address, 'Mapping address must be an Address', blame)
    address_obj = cast(Obj, address)
    if not _BoolProperty(address_obj, 'valid', blame):
        raise error.TypeErr(address, 'Mapping address is not valid', blame)
    return _Token(address_obj, 'Address', blame)


def _FlagStrings(items, blame):
    # type: (List[value_t], loc_t) -> List[str]
    result = []  # type: List[str]
    for item in items:
        if item.tag() != value_e.Str:
            raise error.TypeErr(item, 'Native flags must be text names', blame)
        result.append(cast(value.Str, item).s)
    return result


def _ProtectionMask(items, blame):
    # type: (List[value_t], loc_t) -> int
    mask = 0
    for name in _FlagStrings(items, blame):
        if name == 'none':
            if len(items) != 1:
                raise error.TypeErrVerbose(
                    "'none' cannot be combined with other protection names", blame)
        elif name == 'read':
            mask |= PROT_READ
        elif name == 'write':
            mask |= PROT_WRITE
        elif name == 'execute':
            mask |= PROT_EXECUTE
        else:
            raise error.TypeErrVerbose(
                'Unknown memory protection name %r' % name, blame)
    return mask


def _MappingFlags(items, blame):
    # type: (List[value_t], loc_t) -> int
    mask = 0
    sharing = 0
    for name in _FlagStrings(items, blame):
        if name == 'private':
            mask |= MAP_PRIVATE
            sharing += 1
        elif name == 'shared':
            mask |= MAP_SHARED
            sharing += 1
        elif name == 'anonymous':
            mask |= MAP_ANONYMOUS
        else:
            raise error.TypeErrVerbose('Unknown mapping flag %r' % name, blame)
    if sharing != 1:
        raise error.TypeErrVerbose(
            "Mapping flags require exactly one of 'private' or 'shared'", blame)
    return mask


def _SyncFlags(items, blame):
    # type: (List[value_t], loc_t) -> int
    mask = 0
    kind = 0
    for name in _FlagStrings(items, blame):
        if name == 'sync':
            mask |= SYNC_SYNC
            kind += 1
        elif name == 'async':
            mask |= SYNC_ASYNC
            kind += 1
        elif name == 'invalidate':
            mask |= SYNC_INVALIDATE
        else:
            raise error.TypeErrVerbose('Unknown synchronization flag %r' % name,
                                       blame)
    if kind != 1:
        raise error.TypeErrVerbose(
            "Synchronization flags require exactly one of 'sync' or 'async'",
            blame)
    return mask


def _OpenFlags(items, blame):
    # type: (List[value_t], loc_t) -> int
    mask = 0
    access = 0
    for name in _FlagStrings(items, blame):
        if name == 'read-only':
            mask |= OPEN_READ_ONLY
            access += 1
        elif name == 'write-only':
            mask |= OPEN_WRITE_ONLY
            access += 1
        elif name == 'read-write':
            mask |= OPEN_READ_WRITE
            access += 1
        elif name == 'create':
            mask |= OPEN_CREATE
        elif name == 'exclusive':
            mask |= OPEN_EXCLUSIVE
        elif name == 'truncate':
            mask |= OPEN_TRUNCATE
        elif name == 'append':
            mask |= OPEN_APPEND
        elif name == 'directory':
            mask |= OPEN_DIRECTORY
        elif name == 'no-follow':
            mask |= OPEN_NO_FOLLOW
        elif name == 'close-on-exec':
            mask |= OPEN_CLOSE_ON_EXEC
        else:
            raise error.TypeErrVerbose('Unknown openat flag %r' % name, blame)
    if access != 1:
        raise error.TypeErrVerbose(
            "openat flags require exactly one access mode", blame)
    return mask


def _DefaultProtection():
    # type: () -> List[value_t]
    return [value.Str('read'), value.Str('write')]


def _DefaultMappingFlags():
    # type: () -> List[value_t]
    return [value.Str('private'), value.Str('anonymous')]


def _DefaultSyncFlags():
    # type: () -> List[value_t]
    return [value.Str('sync')]


def _DefaultOpenFlags():
    # type: () -> List[value_t]
    return [value.Str('read-only')]


class Mmap(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        length = rd.PosInt()
        file_value = rd.OptionalValue()
        protection = rd.NamedList('protection', _DefaultProtection())
        flags = rd.NamedList('flags', _DefaultMappingFlags())
        offset = rd.NamedInt('offset', 0)
        rd.Done()

        file_token = -1
        if file_value is not None and file_value.tag() != value_e.Null:
            if file_value.tag() != value_e.Obj:
                raise error.TypeErr(file_value, 'mmap file must be a FileDescriptor',
                                    rd.LeftParenToken())
            file_token = _DescriptorToken(cast(Obj, file_value),
                                          rd.LeftParenToken())

        native_result = libc.grease_mmap(
            mops.ToStr(length),
            _ProtectionMask(protection, rd.LeftParenToken()),
            _MappingFlags(flags, rd.LeftParenToken()), file_token,
            mops.ToStr(offset))
        assert native_result is not None
        errno_num, token = native_result
        if errno_num != 0:
            return _Err(errno_num)
        return _Ok(_Mapping(token, length))


class Munmap(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        mapping = rd.PosObj()
        rd.Done()
        token = _MappingToken(mapping, rd.LeftParenToken())
        errno_num = libc.grease_munmap(token)
        if errno_num != 0:
            return _Err(errno_num)
        mapping.d['active'] = value.Bool(False)
        address = cast(Obj, _Property(mapping, 'address', rd.LeftParenToken()))
        address.d['valid'] = value.Bool(False)
        return _Ok(value.Null)


class Mprotect(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        mapping = rd.PosObj()
        protection = rd.PosList()
        rd.Done()
        token = _MappingToken(mapping, rd.LeftParenToken())
        return _Status(
            libc.grease_mprotect(
                token, _ProtectionMask(protection, rd.LeftParenToken())))


class Msync(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        mapping = rd.PosObj()
        flags = rd.NamedList('flags', _DefaultSyncFlags())
        rd.Done()
        token = _MappingToken(mapping, rd.LeftParenToken())
        return _Status(
            libc.grease_msync(token, _SyncFlags(flags, rd.LeftParenToken())))


class MappingRead(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        mapping = rd.PosObj()
        offset = rd.PosInt()
        length = rd.PosInt()
        rd.Done()
        token = _MappingToken(mapping, rd.LeftParenToken())
        native_result = libc.grease_mapping_read(
            token, mops.ToStr(offset), mops.ToStr(length))
        assert native_result is not None
        errno_num, data = native_result
        if errno_num != 0:
            return _Err(errno_num)
        return _Ok(value.Str(data))


class MappingWrite(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        mapping = rd.PosObj()
        offset = rd.PosInt()
        data = rd.PosStr()
        rd.Done()
        token = _MappingToken(mapping, rd.LeftParenToken())
        return _Status(
            libc.grease_mapping_write(token, mops.ToStr(offset), data))


class OpenAt(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        directory = rd.PosObj()
        path = rd.PosStr()
        flags = rd.NamedList('flags', _DefaultOpenFlags())
        mode = rd.NamedInt('mode', 438)  # 0666
        rd.Done()

        flag_mask = _OpenFlags(flags, rd.LeftParenToken())
        if mops.Greater(mops.ZERO, mode) or mops.Greater(mode,
                                                        mops.IntWiden(4095)):
            raise error.TypeErrVerbose('openat mode must be between 0 and 4095',
                                       rd.LeftParenToken())

        native_result = libc.grease_openat(
            _DescriptorToken(directory, rd.LeftParenToken()), path, flag_mask,
            mops.BigTruncate(mode))
        assert native_result is not None
        errno_num, token = native_result
        if errno_num != 0:
            return _Err(errno_num)
        is_directory = (flag_mask & OPEN_DIRECTORY) != 0
        return _Ok(_FileDescriptor(token, is_directory))


class Close(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        descriptor = rd.PosObj()
        rd.Done()
        if _BoolProperty(descriptor, 'borrowed', rd.LeftParenToken()):
            raise error.TypeErr(descriptor,
                                'Borrowed FileDescriptor cannot be closed',
                                rd.LeftParenToken())
        token = _DescriptorToken(descriptor, rd.LeftParenToken())
        errno_num = libc.grease_close(token)
        if errno_num != 0:
            return _Err(errno_num)
        descriptor.d['closed'] = value.Bool(True)
        return _Ok(value.Null)


class LinkAt(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        old_directory = rd.PosObj()
        old_path = rd.PosStr()
        new_directory = rd.PosObj()
        new_path = rd.PosStr()
        follow = rd.NamedBool('followSymlink', False)
        rd.Done()
        return _Status(
            libc.grease_linkat(
                _DescriptorToken(old_directory, rd.LeftParenToken()), old_path,
                _DescriptorToken(new_directory, rd.LeftParenToken()), new_path,
                follow))


class SymlinkAt(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        target = rd.PosStr()
        directory = rd.PosObj()
        path = rd.PosStr()
        rd.Done()
        return _Status(
            libc.grease_symlinkat(
                target, _DescriptorToken(directory, rd.LeftParenToken()), path))


class UnlinkAt(vm._Callable):

    def __init__(self):
        # type: () -> None
        pass

    def Call(self, rd):
        # type: (typed_args.Reader) -> value_t
        unused_self = rd.PosObj()
        directory = rd.PosObj()
        path = rd.PosStr()
        remove_directory = rd.NamedBool('removeDirectory', False)
        rd.Done()
        return _Status(
            libc.grease_unlinkat(
                _DescriptorToken(directory, rd.LeftParenToken()), path,
                remove_directory))


def MakeNativeObject():
    # type: () -> Obj
    methods = _Props()
    methods['mmap'] = value.BuiltinFunc(Mmap())
    methods['munmap'] = value.BuiltinFunc(Munmap())
    methods['mprotect'] = value.BuiltinFunc(Mprotect())
    methods['msync'] = value.BuiltinFunc(Msync())
    methods['readMapping'] = value.BuiltinFunc(MappingRead())
    methods['writeMapping'] = value.BuiltinFunc(MappingWrite())
    methods['openat'] = value.BuiltinFunc(OpenAt())
    methods['close'] = value.BuiltinFunc(Close())
    methods['linkat'] = value.BuiltinFunc(LinkAt())
    methods['symlinkat'] = value.BuiltinFunc(SymlinkAt())
    methods['unlinkat'] = value.BuiltinFunc(UnlinkAt())

    props = _Props()
    props['cwd'] = _FileDescriptor(0, True, borrowed=True)
    return Obj(Obj(None, methods), props)