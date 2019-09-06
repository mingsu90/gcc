// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Runtime type representation.

package runtime

import (
	"runtime/internal/atomic"
	"runtime/internal/sys"
	"unsafe"
)

type _type struct {
	size       uintptr
	ptrdata    uintptr
	hash       uint32
	kind       uint8
	align      int8
	fieldAlign uint8
	_          uint8

	hashfn  func(unsafe.Pointer, uintptr) uintptr
	equalfn func(unsafe.Pointer, unsafe.Pointer) bool

	gcdata  *byte
	_string *string
	*uncommontype
	ptrToThis *_type
}

func (t *_type) string() string {
	return *t._string
}

// pkgpath returns the path of the package where t was defined, if
// available. This is not the same as the reflect package's PkgPath
// method, in that it returns the package path for struct and interface
// types, not just named types.
func (t *_type) pkgpath() string {
	if u := t.uncommontype; u != nil {
		if u.pkgPath == nil {
			return ""
		}
		return *u.pkgPath
	}
	return ""
}

type method struct {
	name    *string
	pkgPath *string
	mtyp    *_type
	typ     *_type
	tfn     unsafe.Pointer
}

type uncommontype struct {
	name    *string
	pkgPath *string
	methods []method
}

type imethod struct {
	name    *string
	pkgPath *string
	typ     *_type
}

type interfacetype struct {
	typ     _type
	methods []imethod
}

type maptype struct {
	typ        _type
	key        *_type
	elem       *_type
	bucket     *_type // internal type representing a hash bucket
	keysize    uint8  // size of key slot
	valuesize  uint8  // size of value slot
	bucketsize uint16 // size of bucket
	flags      uint32
}

// Note: flag values must match those used in the TMAP case
// in ../cmd/compile/internal/gc/reflect.go:dtypesym.
func (mt *maptype) indirectkey() bool { // store ptr to key instead of key itself
	return mt.flags&1 != 0
}
func (mt *maptype) indirectvalue() bool { // store ptr to value instead of value itself
	return mt.flags&2 != 0
}
func (mt *maptype) reflexivekey() bool { // true if k==k for all keys
	return mt.flags&4 != 0
}
func (mt *maptype) needkeyupdate() bool { // true if we need to update key on an overwrite
	return mt.flags&8 != 0
}
func (mt *maptype) hashMightPanic() bool { // true if hash function might panic
	return mt.flags&16 != 0
}

type arraytype struct {
	typ   _type
	elem  *_type
	slice *_type
	len   uintptr
}

type chantype struct {
	typ  _type
	elem *_type
	dir  uintptr
}

type slicetype struct {
	typ  _type
	elem *_type
}

type functype struct {
	typ       _type
	dotdotdot bool
	in        []*_type
	out       []*_type
}

type ptrtype struct {
	typ  _type
	elem *_type
}

type structfield struct {
	name       *string // nil for embedded fields
	pkgPath    *string // nil for exported Names; otherwise import path
	typ        *_type  // type of field
	tag        *string // nil if no tag
	offsetAnon uintptr // byte offset of field<<1 | isAnonymous
}

func (f *structfield) offset() uintptr {
	return f.offsetAnon >> 1
}

func (f *structfield) anon() bool {
	return f.offsetAnon&1 != 0
}

type structtype struct {
	typ    _type
	fields []structfield
}

// typeDescriptorList holds a list of type descriptors generated
// by the compiler. This is used for the compiler to register
// type descriptors to the runtime.
// The layout is known to the compiler.
//go:notinheap
type typeDescriptorList struct {
	count int
	types [1]uintptr // variable length
}

// typelist holds all type descriptors generated by the comiler.
// This is for the reflect package to deduplicate type descriptors
// when it creates a type that is also a compiler-generated type.
var typelist struct {
	initialized uint32
	lists       []*typeDescriptorList // one element per package
	types       map[string]uintptr    // map from a type's string to *_type, lazily populated
	// TODO: use a sorted array instead?
}
var typelistLock mutex

// The compiler generates a call of this function in the main
// package's init function, to register compiler-generated
// type descriptors.
// p points to a list of *typeDescriptorList, n is the length
// of the list.
//go:linkname registerTypeDescriptors
func registerTypeDescriptors(n int, p unsafe.Pointer) {
	*(*slice)(unsafe.Pointer(&typelist.lists)) = slice{p, n, n}
}

// The reflect package uses this function to look up a compiler-
// generated type descriptor.
//go:linkname reflect_lookupType reflect.lookupType
func reflect_lookupType(s string) *_type {
	// Lazy initialization. We don't need to do this if we never create
	// types through reflection.
	if atomic.Load(&typelist.initialized) == 0 {
		lock(&typelistLock)
		if atomic.Load(&typelist.initialized) == 0 {
			n := 0
			for _, list := range typelist.lists {
				n += list.count
			}
			typelist.types = make(map[string]uintptr, n)
			for _, list := range typelist.lists {
				for i := 0; i < list.count; i++ {
					typ := *(**_type)(add(unsafe.Pointer(&list.types), uintptr(i)*sys.PtrSize))
					typelist.types[typ.string()] = uintptr(unsafe.Pointer(typ))
				}
			}
			atomic.Store(&typelist.initialized, 1)
		}
		unlock(&typelistLock)
	}

	return (*_type)(unsafe.Pointer(typelist.types[s]))
}
