// A C surface over yrs for Notolith's CrdtDoc (libs/ext/share/CrdtDocYrs.cpp).
//
// Why not the upstream C bindings (crate yffi): the crate ships no header and
// is not usable as a dependency (staticlib/cdylib only), and its text API is
// NUL-terminated C strings with unwrap()s, so one U+0000 in a remote update
// aborts the whole process. This surface is UTF-8 with explicit lengths and
// every entry point catches panics, so a hostile or corrupt update is an
// error code, never an abort.
//
// Everything is UTF-16 in offset terms (OffsetKind::Utf16), the unit of
// QString. Buffers handed out are owned by Rust: give them back through
// ncrdt_buf_free.

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use yrs::updates::decoder::Decode;
use yrs::updates::encoder::Encode;
use yrs::{
    Doc, GetString, Observable, OffsetKind, Options, ReadTxn, StateVector, Text, TextRef, Transact,
    Update,
};
use yrs::types::Delta;
use yrs::Out;

const CLIENT_ID_BITS: u64 = (1 << 53) - 1;

pub struct Handle {
    doc: Doc,
    text: TextRef,
    update: Arc<Mutex<Vec<u8>>>,
    delta: Arc<Mutex<Vec<u8>>>,
    // Only a remote apply wants the delta; computing it for a local edit
    // costs a fifth of a keystroke on a big note.
    want_delta: Arc<AtomicBool>,
}

#[repr(C)]
pub struct Buf {
    pub ptr: *mut u8,
    pub len: usize,
}

const OK: i32 = 0;
const ERR_ARG: i32 = 1;
const ERR_DECODE: i32 = 2;
const ERR_PANIC: i32 = 3;

fn give(out: *mut Buf, v: Vec<u8>) {
    let b = v.into_boxed_slice();
    let len = b.len();
    let ptr = Box::into_raw(b) as *mut u8;
    unsafe { *out = Buf { ptr, len } };
}

fn empty(out: *mut Buf) {
    unsafe { *out = Buf { ptr: std::ptr::null_mut(), len: 0 } };
}

// Delta stream the caller reads: per op a tag byte (0 retain, 1 delete,
// 2 insert), a u32 LE length in UTF-16 units, and for an insert a u32 LE
// byte length plus the UTF-8 text.
fn encode_delta(delta: &[Delta], into: &mut Vec<u8>) {
    for d in delta {
        match d {
            Delta::Retain(n, _) => {
                into.push(0);
                into.extend_from_slice(&n.to_le_bytes());
            }
            Delta::Deleted(n) => {
                into.push(1);
                into.extend_from_slice(&n.to_le_bytes());
            }
            Delta::Inserted(Out::Any(yrs::Any::String(s)), _) => {
                into.push(2);
                into.extend_from_slice(&(s.encode_utf16().count() as u32).to_le_bytes());
                into.extend_from_slice(&(s.len() as u32).to_le_bytes());
                into.extend_from_slice(s.as_bytes());
            }
            // An embed or nested type: not text, but it occupies one offset
            // in yrs, so it is passed on as one placeholder character to keep
            // every later offset honest.
            Delta::Inserted(_, _) => {
                into.push(2);
                into.extend_from_slice(&1u32.to_le_bytes());
                into.extend_from_slice(&3u32.to_le_bytes());
                into.extend_from_slice("\u{FFFC}".as_bytes());
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn ncrdt_new(client_id: u64) -> *mut Handle {
    catch_unwind(|| {
        let mut options = Options::with_client_id(yrs::block::ClientID::new(client_id & CLIENT_ID_BITS));
        options.offset_kind = OffsetKind::Utf16;
        let doc = Doc::with_options(options);
        let text = doc.get_or_insert_text("body");
        let update = Arc::new(Mutex::new(Vec::new()));
        let delta = Arc::new(Mutex::new(Vec::new()));
        let u = update.clone();
        let _ = doc.observe_update_v1(1u32, move |_, e| {
            *u.lock().unwrap() = e.update.clone();
        });
        let want_delta = Arc::new(AtomicBool::new(false));
        let (d, want) = (delta.clone(), want_delta.clone());
        text.observe(1u32, move |txn, e| {
            if !want.load(Ordering::Relaxed) {
                return;
            }
            let mut buf = d.lock().unwrap();
            buf.clear();
            encode_delta(e.delta(txn), &mut buf);
        });
        Box::into_raw(Box::new(Handle { doc, text, update, delta, want_delta }))
    })
    .unwrap_or(std::ptr::null_mut())
}

/// # Safety
/// `h` is a handle from ncrdt_new, or null; not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn ncrdt_free(h: *mut Handle) {
    if !h.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(h))));
    }
}

/// # Safety
/// The pair came out of this library unchanged.
#[no_mangle]
pub unsafe extern "C" fn ncrdt_buf_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len)));
    }
}

// yrs reserves a table for the update's claimed client count before reading
// any of them (touching the memory: a 40-byte noise update was seen to cost
// 2 GB and half a second). Every client takes at least three bytes, so a count
// beyond what the bytes could hold is garbage, whatever follows.
fn plausible(update: &[u8]) -> bool {
    let (mut count, mut shift, mut used) = (0u64, 0, 0);
    for b in update.iter().take(5) {
        count |= u64::from(b & 0x7f) << shift;
        shift += 7;
        used += 1;
        if b & 0x80 == 0 {
            return count <= (update.len() - used) as u64 / 3;
        }
    }
    false
}

fn guarded(f: impl FnOnce() -> i32) -> i32 {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(ERR_PANIC)
}

/// # Safety
/// `h` valid; `out` points at a Buf.
#[no_mangle]
pub unsafe extern "C" fn ncrdt_text(h: *mut Handle, out: *mut Buf) -> i32 {
    empty(out);
    guarded(|| {
        let h = &*h;
        let txn = h.doc.transact();
        give(out, h.text.get_string(&txn).into_bytes());
        OK
    })
}

/// A local edit; the update to broadcast comes back in `update` (empty when
/// the edit changed nothing). `at` and `removed` are UTF-16 units and must lie
/// inside the text, else ERR_ARG and the document is untouched.
///
/// # Safety
/// `h` valid; `ins` points at `ins_len` bytes; `update` points at a Buf.
#[no_mangle]
pub unsafe extern "C" fn ncrdt_local_edit(
    h: *mut Handle,
    at: u32,
    removed: u32,
    ins: *const u8,
    ins_len: usize,
    update: *mut Buf,
) -> i32 {
    empty(update);
    guarded(|| {
        let h = &*h;
        let bytes = if ins_len == 0 { &[][..] } else { std::slice::from_raw_parts(ins, ins_len) };
        let Ok(s) = std::str::from_utf8(bytes) else { return ERR_ARG };
        let mut txn = match h.doc.try_transact_mut() {
            Ok(t) => t,
            Err(_) => return ERR_ARG,
        };
        let len = h.text.len(&txn);
        if at > len || removed > len - at {
            return ERR_ARG;
        }
        h.update.lock().unwrap().clear();
        if removed > 0 {
            h.text.remove_range(&mut txn, at, removed);
        }
        if !s.is_empty() {
            h.text.insert(&mut txn, at, s);
        }
        drop(txn); // commit: fires the update observer
        give(update, std::mem::take(&mut *h.update.lock().unwrap()));
        OK
    })
}

/// Apply a remote update; the text delta it caused is returned in `delta`
/// (format above). Anything that does not decode is ERR_DECODE and leaves the
/// document untouched.
///
/// # Safety
/// `h` valid; `upd` points at `len` bytes; `delta` points at a Buf.
#[no_mangle]
pub unsafe extern "C" fn ncrdt_apply(h: *mut Handle, upd: *const u8, len: usize, delta: *mut Buf) -> i32 {
    empty(delta);
    guarded(|| {
        let h = &*h;
        let bytes = if len == 0 { &[][..] } else { std::slice::from_raw_parts(upd, len) };
        if !plausible(bytes) {
            return ERR_DECODE;
        }
        let Ok(update) = Update::decode_v1(bytes) else { return ERR_DECODE };
        let mut txn = match h.doc.try_transact_mut() {
            Ok(t) => t,
            Err(_) => return ERR_ARG,
        };
        h.delta.lock().unwrap().clear();
        h.want_delta.store(true, Ordering::Relaxed);
        let applied = txn.apply_update(update);
        drop(txn); // commit: fires the observers
        h.want_delta.store(false, Ordering::Relaxed);
        if applied.is_err() {
            return ERR_DECODE;
        }
        give(delta, std::mem::take(&mut *h.delta.lock().unwrap()));
        OK
    })
}

/// # Safety
/// `h` valid; `out` points at a Buf.
#[no_mangle]
pub unsafe extern "C" fn ncrdt_state_vector(h: *mut Handle, out: *mut Buf) -> i32 {
    empty(out);
    guarded(|| {
        let h = &*h;
        let txn = h.doc.transact();
        give(out, txn.state_vector().encode_v1());
        OK
    })
}

/// Everything the peer behind `sv` lacks, pending updates included. A null or
/// empty `sv` is a peer that has nothing: the full state.
///
/// # Safety
/// `h` valid; `sv` points at `sv_len` bytes; `out` points at a Buf.
#[no_mangle]
pub unsafe extern "C" fn ncrdt_diff(h: *mut Handle, sv: *const u8, sv_len: usize, out: *mut Buf) -> i32 {
    empty(out);
    guarded(|| {
        let h = &*h;
        let state = if sv_len == 0 {
            StateVector::default()
        } else {
            match StateVector::decode_v1(std::slice::from_raw_parts(sv, sv_len)) {
                Ok(s) => s,
                Err(_) => return ERR_DECODE,
            }
        };
        let txn = h.doc.transact();
        give(out, txn.encode_state_as_update_v1(&state));
        OK
    })
}
