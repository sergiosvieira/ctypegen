/*
   Copyright 2017 Arista Networks.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

       Unless required by applicable law or agreed to in writing, software
       distributed under the License is distributed on an "AS IS" BASIS,
       WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
       See the License for the specific language governing permissions and
       limitations under the License.
*/

#include <Python.h>
#include <structmember.h>

#include <iostream>
#include <deque>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include <libpstack/elf.h>
#include <libpstack/dwarf.h>

namespace {

/*
 * descriptors for the types implemented here.
 */
static PyTypeObject elfObjectType = { PyObject_HEAD_INIT( 0 ) 0 };
static PyTypeObject dwarfEntryType = { PyObject_HEAD_INIT( 0 ) 0 };
static PyTypeObject dwarfEntryIteratorType = { PyObject_HEAD_INIT( 0 ) 0 };
static PyTypeObject dwarfTagsType = { PyObject_HEAD_INIT( 0 ) 0 };
static PyTypeObject dwarfAttrsType = { PyObject_HEAD_INIT( 0 ) 0 };

// These are the DWARF tags for DIEs that introduce a new namespace in C/C++
static std::set< Dwarf::Tag > namespacetags = {
   Dwarf::DW_TAG_structure_type,
   Dwarf::DW_TAG_namespace,
   Dwarf::DW_TAG_class_type,
   Dwarf::DW_TAG_union_type,
};

/*
 * Return the name of a DIE.
 * If the DIE has a name attribute, that's returned.
 * If not, we fabricate an anonymous name based on the DIEs offset.
 */
static std::string
dieName( const Dwarf::DIE & die ) {
   const Dwarf::Attribute & name = die.attribute( Dwarf::DW_AT_name );
   if ( name.valid() )
      return std::string( name );
   std::ostringstream os;
   os << "anon_" << die.getOffset();
   return os.str();
}

/*
 * For DIE nested in namespaces, construct a sequence in a std container for
 * it's name and containing namespaces, from outer to inner.
 */
template< typename container >
static void
getFullName( const Dwarf::DIE & die, container & fullname, bool leaf = true ) {
   if ( die.getParentOffset() != 0 ) {
      const Dwarf::DIE & parent =
         die.getUnit()->offsetToDIE( die.getParentOffset() );
      getFullName( parent, fullname, false );
   }
   if ( leaf || namespacetags.find( die.tag() ) != namespacetags.end() ) {
      fullname.push_back( dieName( die ) );
   }
}

/*
 * DIEs with the DW_AT_declaration attribute set are indicative of an incomplete
 * type (eg, "struct foo;". Typedefs can refer to such DIEs, in which case
 * we need to find the actual definition to fulfill the output of the typedef.
 * "findDefinition" finds a defining DIE (one with no DW_AT_declaration attribute)
 * for a declaration DIE with the same name/scope.
 *
 * arguments:
 * die: the node of the tree we wish to search.
 * tag: the tag of the original DIE
 * first/end: the remaining scopes in the DIEs name (when die is the root node, these
 * are the name of the DIE we want split into its component namespaces)
 */
template< typename T >
static Dwarf::DIE
findDefinition( const Dwarf::DIE & die,
                Dwarf::Tag tag,
                typename T::iterator first,
                typename T::iterator end ) {
   const auto & nameA = die.attribute( Dwarf::DW_AT_name );
   const bool sameName = nameA.valid() && std::string( nameA ) == *first;

   if ( end - first == 1 ) {
      /*
       * We've decended all the namespaces - this is the leaf of the name.  We
       * have a match if its the right name, if the DIE we're looking at is not
       * a declaration, and it's also got the same type as what we're looking
       * for.
       */
      const auto & declA = die.attribute( Dwarf::DW_AT_declaration );
      if ( sameName && !bool( declA ) && tag == die.tag() )
         return die;
   }

   /*
    * If the current DIE is a namespace, and the name matches the next namesspace
    * we are intereted in, then descend down it.
    */
   switch ( die.tag() ) {
    case Dwarf::DW_TAG_namespace:
    case Dwarf::DW_TAG_structure_type:
    case Dwarf::DW_TAG_class_type:
      if ( !sameName )
         return Dwarf::DIE();
      first++;
      // FALLTHROUGH

    case Dwarf::DW_TAG_compile_unit:
      // Compile units are a bit special - we just fall into them, but they don't
      // consume a namespace.
      for ( const auto c : die.children() ) {
         const auto ischild = findDefinition< T >( c, tag, first, end );
         if ( ischild )
            return ischild;
      }
      break;
    default:
      break;
   }
   return Dwarf::DIE();
}

/*
 * Convert C++ string to python string.
 */
static PyObject *
makeString( const std::string & s ) {
   return PyUnicode_FromString( s.c_str() );
}

} // namespace

extern "C" {

/*
 * Python representaiton of a loaded ELF object and it's DWARF debug data.
 */
typedef struct {
   PyObject_HEAD std::shared_ptr< Elf::Object > obj;
   std::shared_ptr< Dwarf::Info > dwarf;
} PyElfObject;

/*
 * Tabulate objects, members, and init functions for "attrs" and "types" objects
 * inside the libCTypeGen namespace that can be used to access the DWARF attribute
 * and tags values symbolically.
 */
typedef struct {
   PyObject_HEAD
#define DWARF_ATTR( name, value ) int name;
#include <libpstack/dwarf/attr.h>
#undef DWARF_ATTR
} PyDwarfAttrsObject;

typedef struct {
   PyObject_HEAD
#define DWARF_TAG( name, value ) int name;
#include <libpstack/dwarf/tags.h>
#undef DWARF_TAG
} PyDwarfTagsObject;

struct PyMemberDef attr_members[] = {
#define DWARF_ATTR( name, value )                                                   \
   { ( char * )#name,                                                               \
     T_INT,                                                                         \
     offsetof( PyDwarfAttrsObject, name ),                                          \
     0,                                                                             \
     ( char * )#name },
#include <libpstack/dwarf/attr.h>
#undef DWARF_ATTR
   { NULL }
};

struct PyMemberDef tag_members[] = {
#define DWARF_TAG( name, value )                                                    \
   { ( char * )#name,                                                               \
     T_INT,                                                                         \
     offsetof( PyDwarfTagsObject, name ),                                           \
     0,                                                                             \
     ( char * )#name },
#include <libpstack/dwarf/tags.h>
#undef DWARF_TAG
   { NULL }
};

static int
attr_init( PyObject * object, PyObject * args, PyObject * kwds ) {
   auto attrs = ( PyDwarfAttrsObject * )object;
#define DWARF_ATTR( name, value ) attrs->name = value;
#include <libpstack/dwarf/attr.h>
#undef DWARF_ATTR
   return 0;
};

static int
tags_init( PyObject * object, PyObject * args, PyObject * kwds ) {
   auto tags = ( PyDwarfTagsObject * )object;
#define DWARF_TAG( name, value ) tags->name = value;
#include <libpstack/dwarf/tags.h>
#undef DWARF_TAG
   return 0;
};

/*
 * Python representaiton of a DWARF information entry. (a DIE)
 */
typedef struct {
   PyObject_HEAD Dwarf::DIE die;
} PyDwarfEntry;

static PyObject *
makeEntry( const Dwarf::DIE & die ) {
   PyDwarfEntry * value = PyObject_New( PyDwarfEntry, &dwarfEntryType );
   value->die = die;
   return ( PyObject * )value;
}

/*
 * Python representation of an iterator over the child DIEs of a parent DIE
 */
typedef struct {
   PyObject_HEAD const Dwarf::Unit * unit;
   Dwarf::DIEIter begin;
   Dwarf::DIEIter end;
} PyDwarfEntryIterator;

static PyObject *
elf_open( PyObject * self, PyObject * args ) {
   static Dwarf::ImageCache imageCache;
   try {
      const char * image;
      if ( !PyArg_ParseTuple( args, "s", &image ) )
         return nullptr;
      auto dwarf = imageCache.getDwarf( image );
      auto obj = dwarf->elf;
      PyElfObject * val = PyObject_New( PyElfObject, &elfObjectType );
      new ( &val->obj ) std::shared_ptr< Elf::Object >( obj );
      new ( &val->dwarf ) std::shared_ptr< Dwarf::Info >( dwarf );
      val->dwarf = imageCache.getDwarf( image );
      val->obj = val->dwarf->elf;
      return ( PyObject * )val;
   } catch ( const std::exception & ex ) {
      PyErr_SetString( PyExc_RuntimeError, ex.what() );
      return nullptr;
   }
}

static PyObject *
elf_units( PyObject * self, PyObject * args ) {
   try {
      PyElfObject * pye = ( PyElfObject * )self;
      const auto & units = pye->dwarf->getUnits();
      PyObject * result = PyList_New( units.size() );
      size_t i = 0;
      for ( auto unit : units )
         PyList_SetItem( result, i++, makeEntry( *unit->topLevelDIEs().begin() ) );
      return result;
   } catch ( const std::exception & ex ) {
      PyErr_SetString( PyExc_RuntimeError, ex.what() );
      return nullptr;
   }
}

static PyObject *
elf_findDefinition( PyObject * self, PyObject * args ) {
   PyDwarfEntry * die;
   PyElfObject * elf = ( PyElfObject * )self;
   if ( !PyArg_ParseTuple( args, "O", &die ) )
      return nullptr;
   std::vector< std::string > namelist;
   getFullName( die->die, namelist );
   for ( const auto & u : elf->dwarf->getUnits() ) {
      for ( const auto & tld : u->topLevelDIEs() ) {
         const auto & defn = findDefinition< std::vector< std::string > >(
            tld, die->die.tag(), namelist.begin(), namelist.end() );
         if ( defn )
            return makeEntry( defn );
      }
   }
   Py_INCREF( Py_None );
   return Py_None;
}

static void
elf_free( PyObject * o ) {
   PyElfObject * pye = ( PyElfObject * )o;
   pye->obj.std::shared_ptr< Elf::Object >::~shared_ptr< Elf::Object >();
   pye->dwarf.std::shared_ptr< Dwarf::Info >::~shared_ptr< Dwarf::Info >();
   elfObjectType.tp_free( o );
}

static PyObject *
entry_type( PyObject * self, PyObject * args ) {
   PyDwarfEntry * ent = ( PyDwarfEntry * )self;
   return PyLong_FromLong( ent->die.tag() );
}

/*
 * DIEs have offsets within their unit, and the units have offsets within the
 * DWARF section they are defined in.
 * We compare two dies by comparing the offsets of their units first, and then
 * the offsets of the DIEs themselves.
 */
static PyObject *
entry_compare( PyObject * lhso, PyObject * rhso, int op ) {
   if ( Py_TYPE( rhso ) != &dwarfEntryType ) {
      Py_INCREF( Py_False );
      return Py_False;
   }

   PyDwarfEntry * lhs = ( PyDwarfEntry * )lhso;
   PyDwarfEntry * rhs = ( PyDwarfEntry * )rhso;

   size_t diff = lhs->die.getUnit()->offset - rhs->die.getUnit()->offset;
   if ( diff == 0 )
      diff = lhs->die.getOffset() - rhs->die.getOffset();

   auto pythonBool = []( bool cbool ) {
      PyObject * pybool = cbool ? Py_True : Py_False;
      Py_INCREF( pybool );
      return pybool;
   };

   if ( Py_TYPE( rhs ) == Py_TYPE( lhs ) ) {
      switch ( op ) {
       case Py_EQ:
         return pythonBool( diff == 0 );
       case Py_NE:
         return pythonBool( diff != 0 );
       case Py_GT:
         return pythonBool( diff > 0 );
       case Py_GE:
         return pythonBool( diff >= 0 );
       case Py_LT:
         return pythonBool( diff < 0 );
       case Py_LE:
         return pythonBool( diff <= 0 );
       default:
         break;
      }
   }
   Py_INCREF( Py_NotImplemented );
   return Py_NotImplemented;
}

#if PY_MAJOR_VERSION >= 3
typedef Py_hash_t hashfunc_result;
#else
typedef long hashfunc_result;
#endif
hashfunc_result
entry_hash( PyObject * self ) {
   PyDwarfEntry * ent = ( PyDwarfEntry * )self;
   return hashfunc_result( ent->die.getOffset() ^ ent->die.getUnit()->offset );
}

/*
 * Provide an iterator over the children of a DIE.
 */
static PyObject *
entry_iterator( PyObject * self ) {
   try {
      PyDwarfEntry * ent = ( PyDwarfEntry * )self;
      PyDwarfEntryIterator * it =
         PyObject_New( PyDwarfEntryIterator, &dwarfEntryIteratorType );
      Dwarf::DIEList list = ent->die.children();
      new ( &it->begin ) Dwarf::DIEIter( list.begin() );
      new ( &it->end ) Dwarf::DIEIter( list.end() );
      it->unit = ent->die.getUnit();
      return ( PyObject * )it;
   } catch ( const std::exception & ex ) {
      PyErr_SetString( PyExc_RuntimeError, ex.what() );
      return nullptr;
   }
}

/*
 * Return the local name of the entry
 */
static PyObject *
entry_name( PyObject * self, PyObject * args ) {
   PyDwarfEntry * ent = ( PyDwarfEntry * )self;
   return makeString( dieName( ent->die ) );
}

/*
 * Return the offset of the entry
 */
static PyObject *
entry_offset( PyObject * self, PyObject * args ) {
   PyDwarfEntry * ent = ( PyDwarfEntry * )self;
   return PyLong_FromLong( ent->die.getOffset() );
}

/*
 * Return the name of the file containing the DIE.
 */
static PyObject *
entry_file( PyObject * self, PyObject * args ) {
   PyDwarfEntry * ent = ( PyDwarfEntry * )self;
   std::string txt = stringify( *ent->die.getUnit()->dwarf->elf->io );
   return makeString( txt );
}

/*
 * Get an attribute in the DIE
 * To make it easy to use for python, we convert the integer index to a DWARF
 * attribute name. (The "attrs" object in the module contains the numeric values for
 * the named DWARF attrs.)
 *
 * We use this for both the indexing operation on the DIe, and explicitly with the
 * getattr method
 */
static PyObject *
entry_getattr_idx( PyObject * self, Py_ssize_t idx ) {
   try {
      const auto pyEntry = ( PyDwarfEntry * )self;
      const Dwarf::Attribute & attr =
         pyEntry->die.attribute( Dwarf::AttrName( idx ) );
      if ( !attr.valid() )
         Py_RETURN_NONE;
      switch ( attr.form() ) {
       case Dwarf::DW_FORM_addr:
         return PyLong_FromUnsignedLongLong( uintmax_t( attr ) );
       case Dwarf::DW_FORM_data1:
       case Dwarf::DW_FORM_data2:
       case Dwarf::DW_FORM_data4:
         return PyLong_FromLong( intmax_t( attr ) );
       case Dwarf::DW_FORM_sdata:
       case Dwarf::DW_FORM_data8:
         return PyLong_FromLongLong( intmax_t( attr ) );
       case Dwarf::DW_FORM_udata:
         return PyLong_FromUnsignedLongLong( uintmax_t( attr ) );
       case Dwarf::DW_FORM_GNU_strp_alt:
       case Dwarf::DW_FORM_string:
       case Dwarf::DW_FORM_strp:
         return makeString( std::string( attr ) );
       case Dwarf::DW_FORM_ref1:
       case Dwarf::DW_FORM_ref2:
       case Dwarf::DW_FORM_ref4:
       case Dwarf::DW_FORM_ref8:
       case Dwarf::DW_FORM_ref_udata:
       case Dwarf::DW_FORM_GNU_ref_alt:
       case Dwarf::DW_FORM_ref_addr:
         return makeEntry( Dwarf::DIE( attr ) );
       case Dwarf::DW_FORM_flag_present:
         Py_RETURN_TRUE;
       case Dwarf::DW_FORM_flag:
         if ( bool( attr ) ) {
            Py_RETURN_TRUE;
         } else {
            Py_RETURN_FALSE;
         }
       default:
         std::clog << "no handler for form " << attr.form() << "in attribute " << idx
                   << "\n";
         break;
      }
      Py_RETURN_NONE;
   } catch ( const std::exception & ex ) {
      PyErr_SetString( PyExc_RuntimeError, ex.what() );
      return nullptr;
   }
}

static void
entry_free( PyObject * o ) {
   dwarfEntryType.tp_free( o );
}

/*
 * Return the fully-qualified name of the entry as a tuple, with one item for
 * each namespace
 */
static PyObject *
entry_fullname( PyObject * self, PyObject * args ) {
   std::deque< std::string > namelist;
   PyDwarfEntry * ent = ( PyDwarfEntry * )self;
   const Dwarf::DIE & die = ent->die;
   getFullName( die, namelist );
   PyObject * tuple = PyTuple_New( namelist.size() );
   size_t i = 0;
   for ( const auto & item : namelist ) {
      PyTuple_SET_ITEM( tuple, i, makeString( item ) );
      i++;
   }
   return tuple;
}

/*
 * Get next DIE in a parent's iterator
 */
static PyObject *
entryiter_iternext( PyObject * self ) {
   PyDwarfEntryIterator * it = ( PyDwarfEntryIterator * )self;
   if ( it->begin == it->end ) {
      PyErr_SetNone( PyExc_StopIteration );
      return nullptr;
   }
   PyObject * rv = makeEntry( *it->begin );
   ++it->begin;
   return rv;
}

static PyObject *
entryiter_iter( PyObject * self ) {
   Py_INCREF( self );
   return self;
}

static void
entryiter_free( PyObject * o ) {
   PyDwarfEntryIterator * it = ( PyDwarfEntryIterator * )o;
   it->begin.Dwarf::DIEIter::~DIEIter();
   it->end.Dwarf::DIEIter::~DIEIter();
   elfObjectType.tp_free( o );
}

static PyMethodDef ctypegen_methods[] = {
   { "open", elf_open, METH_VARARGS, "open an ELF file to process" }, { 0, 0, 0, 0 }
};

static PyMethodDef elf_methods[] = {
   { "units", elf_units, METH_VARARGS, "get a list of unit-level DWARF entries" },
   { "findDefinition",
     elf_findDefinition,
     METH_VARARGS,
     "Given a DIE for a declaration, find a definition DIE with the same name" },
   { 0, 0, 0, 0 }
};

static PyMethodDef entry_methods[] = {
   { "tag", entry_type, METH_VARARGS, "get type of a DIE" },
   { "offset", entry_offset, METH_VARARGS, "offset of a DIE in DWARF image" },
   { "file", entry_file, METH_VARARGS, "file containing DIE" },
   { "name", entry_name, METH_VARARGS, "get namespace-local name of a DIE" },
   { "fullname",
     entry_fullname,
     METH_VARARGS,
     "get full name of a DIE (as tuple, with entry for each namesace)" },
   { 0, 0, 0, 0 }
};

static PySequenceMethods entry_sequence = {
   nullptr, nullptr, nullptr, entry_getattr_idx
};

PyMODINIT_FUNC
#if PY_MAJOR_VERSION >= 3
PyInit_libCTypeGen( void )
#else
initlibCTypeGen( void )
#endif
{
#if PY_MAJOR_VERSION >= 3

   static struct PyModuleDef ctypeGenModule = {
      PyModuleDef_HEAD_INIT,
      "libCTypeGen", /* m_name */
      "ELF/DWARF helper library", /* m_doc */
      -1, /* m_size */
      ctypegen_methods, /* m_methods */
      NULL, /* m_reload */
      NULL, /* m_traverse */
      NULL, /* m_clear */
      NULL, /* m_free */
   };

   PyObject * module = PyModule_Create( &ctypeGenModule );
#else
   PyObject * module =
      Py_InitModule3( "libCTypeGen", ctypegen_methods, "ELF helpers" );
#endif

   dwarfAttrsType.tp_name = "DWARFAttrs";
   dwarfAttrsType.tp_flags = Py_TPFLAGS_DEFAULT;
   dwarfAttrsType.tp_basicsize = sizeof( PyDwarfAttrsObject );
   dwarfAttrsType.tp_methods = nullptr;
   dwarfAttrsType.tp_doc = "python attribute names";
   dwarfAttrsType.tp_members = attr_members;
   dwarfAttrsType.tp_dealloc = nullptr;
   dwarfAttrsType.tp_init = attr_init;

   dwarfTagsType.tp_name = "DWARFTags";
   dwarfTagsType.tp_flags = Py_TPFLAGS_DEFAULT;
   dwarfTagsType.tp_basicsize = sizeof( PyDwarfTagsObject );
   dwarfTagsType.tp_methods = nullptr;
   dwarfTagsType.tp_doc = "python tag names";
   dwarfTagsType.tp_members = tag_members;
   dwarfTagsType.tp_dealloc = nullptr;
   dwarfTagsType.tp_init = tags_init;

   if ( PyType_Ready( &dwarfTagsType ) >= 0 ) {
      auto tags = PyObject_New( PyObject, &dwarfTagsType );
      tags->ob_type->tp_init( tags, nullptr, nullptr );
      PyModule_AddObject( module, "tags", ( PyObject * )tags );
   }
   if ( PyType_Ready( &dwarfAttrsType ) >= 0 ) {
      auto attrs = PyObject_New( PyObject, &dwarfAttrsType );
      attrs->ob_type->tp_init( attrs, nullptr, nullptr );
      PyModule_AddObject( module, "attrs", ( PyObject * )attrs );
   }

   elfObjectType.tp_name = "libCTypeGen.ElfObject";
   elfObjectType.tp_flags = Py_TPFLAGS_DEFAULT;
   elfObjectType.tp_basicsize = sizeof( PyElfObject );
   elfObjectType.tp_methods = elf_methods;
   elfObjectType.tp_doc = "ELF object";
   elfObjectType.tp_dealloc = elf_free;
   elfObjectType.tp_new = PyType_GenericNew;

   dwarfEntryType.tp_name = "libCTypeGen.DwarfEntry";
   dwarfEntryType.tp_flags = Py_TPFLAGS_DEFAULT;
   dwarfEntryType.tp_basicsize = sizeof( PyDwarfEntry );
   dwarfEntryType.tp_doc = "DWARF DIE object";
   dwarfEntryType.tp_dealloc = entry_free;
   dwarfEntryType.tp_new = PyType_GenericNew;
   dwarfEntryType.tp_methods = entry_methods;
   dwarfEntryType.tp_iter = entry_iterator;
   dwarfEntryType.tp_hash = entry_hash;
   dwarfEntryType.tp_richcompare = entry_compare;
   dwarfEntryType.tp_as_sequence = &entry_sequence;

   dwarfEntryIteratorType.tp_name = "libCTypeGen.DwarfEntryIterator";
   dwarfEntryIteratorType.tp_flags = Py_TPFLAGS_DEFAULT;
   dwarfEntryIteratorType.tp_basicsize = sizeof( PyDwarfEntryIterator );
   dwarfEntryIteratorType.tp_doc = "DWARF DIE object iterator";
   dwarfEntryIteratorType.tp_dealloc = entryiter_free;
   dwarfEntryIteratorType.tp_new = PyType_GenericNew;
   dwarfEntryIteratorType.tp_iter = entryiter_iter;
   dwarfEntryIteratorType.tp_iternext = entryiter_iternext;

   if ( PyType_Ready( &elfObjectType ) >= 0 ) {
      Py_INCREF( &elfObjectType );
      PyModule_AddObject( module, "ElfObject", ( PyObject * )&elfObjectType );
   }
   if ( PyType_Ready( &dwarfEntryType ) >= 0 ) {
      Py_INCREF( &dwarfEntryType );
      PyModule_AddObject( module, "DwarfEntry", ( PyObject * )&dwarfEntryType );
   }

#if PY_MAJOR_VERSION >= 3
   return module;
#endif
}
}
