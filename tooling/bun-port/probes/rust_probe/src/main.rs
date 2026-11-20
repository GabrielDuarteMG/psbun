// PS5Term - probe_rust: Rust std on a PS5 payload (threads, thread_local with destructors,
// files, metadata, readdir, env, time, process spawning, panics caught by abort policy).
// GPL-3.0-or-later.
use std::cell::Cell;
use std::io::{Read, Write};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

static DROPS: AtomicUsize = AtomicUsize::new(0);
struct Tracker(Cell<u32>);
impl Drop for Tracker {
    fn drop(&mut self) { DROPS.fetch_add(1, Ordering::SeqCst); }
}
thread_local! { static TRACKER: Tracker = Tracker(Cell::new(0)); }

fn check(name: &str, ok: bool) -> bool {
    println!("{} {}", if ok { "ok" } else { "FAIL" }, name);
    ok
}

fn main() {
    let mut all = true;
    let counter = Arc::new(Mutex::new(0u32));
    let handles: Vec<_> = (0..4).map(|i| {
        let c = counter.clone();
        std::thread::spawn(move || {
            TRACKER.with(|t| t.0.set(i));
            for _ in 0..1000 { *c.lock().unwrap() += 1; }
            TRACKER.with(|t| t.0.get())
        })
    }).collect();
    let ids: Vec<u32> = handles.into_iter().map(|h| h.join().unwrap()).collect();
    all &= check("threads+mutex", *counter.lock().unwrap() == 4000 && ids == vec![0, 1, 2, 3]);
    all &= check("thread_local drops", DROPS.load(Ordering::SeqCst) == 4);

    let dir = std::path::Path::new("/data/ps5term-dev/rust-probe");
    let _ = std::fs::remove_dir_all(dir);
    std::fs::create_dir_all(dir).unwrap();
    let file = dir.join("a.txt");
    std::fs::File::create(&file).unwrap().write_all(b"hello ps5").unwrap();
    let mut text = String::new();
    std::fs::File::open(&file).unwrap().read_to_string(&mut text).unwrap();
    let meta = std::fs::metadata(&file).unwrap();
    all &= check("file io", text == "hello ps5");
    all &= check("metadata", meta.len() == 9 && meta.is_file() && meta.modified().is_ok());
    let names: Vec<_> = std::fs::read_dir(dir).unwrap().map(|e| e.unwrap().file_name()).collect();
    all &= check("read_dir", names.len() == 1 && names[0] == "a.txt");
    std::fs::rename(&file, dir.join("b.txt")).unwrap();
    all &= check("rename", dir.join("b.txt").exists() && !file.exists());
    std::fs::remove_dir_all(dir).unwrap();
    all &= check("remove_dir_all", !dir.exists());

    all &= check("env", std::env::var("HOME").map(|h| !h.is_empty()).unwrap_or(false));
    let start = std::time::Instant::now();
    std::thread::sleep(std::time::Duration::from_millis(20));
    let elapsed = start.elapsed().as_millis();
    all &= check("time", (18..1000).contains(&elapsed) && std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH).unwrap().as_secs() > 1_700_000_000);
    all &= check("parallelism", std::thread::available_parallelism().map(|n| n.get() >= 1).unwrap_or(false));
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    let rc = unsafe { libc::stat(b"/data\0".as_ptr() as *const _, &mut st) };
    all &= check("libc::stat layout", rc == 0 && (st.st_mode & libc::S_IFMT) == libc::S_IFDIR);
    if let Some(child) = std::env::args().nth(1) {
        let out = std::process::Command::new(&child).arg("hello").env("PROBE_CHILD", "1").output();
        match out {
            Ok(o) => {
                let ok = o.status.success()
                    && String::from_utf8_lossy(&o.stdout) == "child-ok hello PROBE_CHILD=1\n";
                if !ok {
                    println!("spawn: status {:?} stdout {:?} stderr {:?}", o.status,
                             String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr));
                }
                all &= check("process spawn", ok);
            }
            Err(e) => { println!("spawn error: {e}"); all = false; }
        }
    }
    println!("{}", if all { "ALL OK" } else { "SOME FAILED" });
    std::process::exit(if all { 0 } else { 1 });
}
