use std::collections::HashMap;

/// Index into SourceAtomSet.atoms.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SourceAtomSetIndex {
    index: usize,
}
impl SourceAtomSetIndex {
    fn new(index: usize) -> Self {
        Self { index }
    }

    pub fn into_raw(self) -> usize {
        self.index
    }
}

// Call $handler macro with the list of common atoms.
//
// Basic Usage:
//
// ```
// for_all_common_atoms!(test);
// ```
//
// is expanded to
//
// ```
// test!(
//   ("arguments", arguments, Arguments),
//   ("async", async_, Async),
//   ...
// );
// ```
//
// Extra Parameters:
//
// ```
// for_all_common_atoms!(test, foo, bar);
// ```
//
// is expanded to
//
// ```
// test!(
//   foo, bar,
//   ("arguments", arguments, Arguments),
//   ("async", async_, Async),
//   ...
// );
// ```
macro_rules! for_all_common_atoms {
    ($handler:ident $(, $($params:tt)*)?) => {
        // string representation, method name, enum variant
        $handler!(
            $($($params)*,)?
            ("arguments", arguments, Arguments),
            ("async", async_, Async),
            ("await", await_, Await),
            ("break", break_, Break),
            ("case", case, Case),
            ("catch", catch, Catch),
            ("class", class, Class),
            ("const", const_, Const),
            ("continue", continue_, Continue),
            ("debugger", debugger, Debugger),
            ("default", default, Default),
            ("delete", delete, Delete),
            ("do", do_, Do),
            ("else", else_, Else),
            ("enum", enum_, Enum),
            ("eval", eval, Eval),
            ("export", export, Export),
            ("extends", extends, Extends),
            ("false", false_, False),
            ("finally", finally, Finally),
            ("for", for_, For),
            ("function", function, Function),
            ("if", if_, If),
            ("implements", implements, Implements),
            ("import", import, Import),
            ("in", in_, In),
            ("instanceof", instanceof, Instanceof),
            ("interface", interface, Interface),
            ("let", let_, Let),
            ("new", new_, New),
            ("null", null, Null),
            ("package", package, Package),
            ("private", private, Private),
            ("protected", protected, Protected),
            ("public", public, Public),
            ("return", return_, Return),
            ("static", static_, Static),
            ("super", super_, Super),
            ("switch", switch, Switch),
            ("this", this, This),
            ("throw", throw, Throw),
            ("true", true_, True),
            ("try", try_, Try),
            ("typeof", typeof_, Typeof),
            ("var", var, Var),
            ("void", void, Void),
            ("while", while_, While),
            ("with", with, With),
            ("yield", yield_, Yield),
            ("use strict", use_strict, UseStrict),
            ("__proto__", __proto__, Proto),
        );
    }
}

// Define CommonAtoms enum, with
//
// enum CommonAtoms {
//   Arguments = 0,
//   Async,
//   ...
// }
macro_rules! define_enum {
    (($s0:tt, $method0:ident, $variant0:ident),
     $(($s:tt, $method:ident, $variant:ident),)*) => {
        enum CommonAtoms {
            $variant0 = 0,
            $($variant,)*
        }
    };
}
for_all_common_atoms!(define_enum);

// Define CommonSourceAtomSetIndices struct.
//
// impl CommonSourceAtomSetIndices {
//   pub fn arguments() -> SourceAtomSetIndex { ... }
//   pub fn async_() -> SourceAtomSetIndex { ... }
//   ...
// }
macro_rules! define_struct {
    ($(($s:tt, $method:ident, $variant:ident),)*) => {
        #[derive(Debug)]
        pub struct CommonSourceAtomSetIndices {}

        impl CommonSourceAtomSetIndices {
            $(
                pub fn $method() -> SourceAtomSetIndex {
                    SourceAtomSetIndex::new(CommonAtoms::$variant as usize)
                }
            )*
        }
    };
}
for_all_common_atoms!(define_struct);

/// Set of atoms, including the following:
///
///   * atoms referred from bytecode
///   * variable names referred from scope data
///
/// WARNING: This set itself does *NOT* map to JSScript::atoms().
#[derive(Debug)]
pub struct SourceAtomSet<'alloc> {
    atoms: Vec<String>,

    /// Cache for the case the same string is inserted multiple times.
    atom_indices: HashMap<&'alloc str, SourceAtomSetIndex>,
}

impl<'alloc> SourceAtomSet<'alloc> {
    // Create a set, with all common atoms inserted.
    pub fn new() -> Self {
        let mut result = Self {
            atoms: Vec::new(),
            atom_indices: HashMap::new(),
        };
        result.insert_common_atoms();
        result
    }

    // Insert all common atoms.
    fn insert_common_atoms(&mut self) {
        macro_rules! insert_atom {
            ($self: ident,
             $(($s:tt, $method:ident, $variant:ident),)*) => {
                $(
                    $self.atoms.push($s.to_string());
                    $self
                        .atom_indices
                        .insert($s, CommonSourceAtomSetIndices::$method());
                )*
            };
        }
        for_all_common_atoms!(insert_atom, self);
    }

    // Create a set, without any common atoms.
    //
    // This is for moving `SourceAtomSet` out of `Rc<RefCell>`, by replacing
    // it with the result of this method.
    pub fn new_uninitialized() -> Self {
        Self {
            atoms: Vec::new(),
            atom_indices: HashMap::new(),
        }
    }

    pub fn insert(&mut self, s: &'alloc str) -> SourceAtomSetIndex {
        match self.atom_indices.get(s) {
            Some(index) => return *index,
            _ => {}
        }

        let index = self.atoms.len();
        self.atoms.push(s.to_string());
        let result = SourceAtomSetIndex::new(index);
        self.atom_indices.insert(s, result);
        result
    }

    pub fn get(&self, index: SourceAtomSetIndex) -> String {
        self.atoms[index.into_raw()].clone()
    }

    pub fn into_vec(self) -> Vec<String> {
        self.atoms
    }
}
