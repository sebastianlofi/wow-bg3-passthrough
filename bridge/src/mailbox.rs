use crate::wire::{Error, MAX_FRAME, Role};
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::{
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    path::{Path, PathBuf},
};
/// Two bounded rolling files. Truncated or checksum-invalid reads are retried, never delivered.
/// Use only a dedicated private directory, with exactly one writer per role.
pub struct Mailbox {
    root: PathBuf,
    role: Role,
}
impl Mailbox {
    pub fn new(root: &Path, role: Role) -> Result<Self, Error> {
        let meta = fs::symlink_metadata(root).map_err(|e| e.to_string())?;
        if !meta.is_dir() || meta.permissions().mode() & 0o077 != 0 {
            return Err("mailbox must be a private real directory".into());
        }
        Ok(Self {
            root: root.into(),
            role,
        })
    }
    fn path(&self, role: Role) -> PathBuf {
        self.root.join(role.file())
    }
    pub fn write(&self, bytes: &[u8]) -> Result<(), Error> {
        if bytes.len() > MAX_FRAME {
            return Err("frame too large".into());
        }
        let path = self.path(self.role);
        if let Ok(m) = fs::symlink_metadata(&path)
            && !m.is_file()
        {
            return Err("mailbox slot is not a regular file".into());
        }
        // The directory is private; a peer is trusted to write only its own slot.
        let mut f = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(path)
            .map_err(|e| e.to_string())?;
        f.write_all(bytes).map_err(|e| e.to_string())
    }
    pub fn read(&self) -> Result<Option<Vec<u8>>, Error> {
        let path = self.path(self.role.other());
        match fs::symlink_metadata(&path) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(e) => return Err(e.to_string()),
            Ok(m) if !m.is_file() => return Err("peer slot is not a regular file".into()),
            _ => {}
        }
        let f = File::open(path).map_err(|e| e.to_string())?;
        let mut bytes = Vec::new();
        f.take((MAX_FRAME + 1) as u64)
            .read_to_end(&mut bytes)
            .map_err(|e| e.to_string())?;
        if bytes.len() > MAX_FRAME {
            return Err("frame too large".into());
        }
        Ok(Some(bytes))
    }
}
