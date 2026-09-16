#!/usr/bin/env python3
"""
pack_assets.py — Pack Sonic R PC retail assets into a case-tolerant tar.gz for Android.

Creates an archive where:
- All directory and file paths exist in UPPERCASE (matching sonicr_paths.h).
- Lowercase symlinks exist for all directories and files for case insensitivity.
- Root files (sonicr.inf, joystick.inf) are included.
"""

import os
import sys
import tarfile

FOLDERS = [
    'general', 'bin', 'ai', 'city', 'emerald',
    'factory', 'ghost', 'island', 'ruin', 'save', 'sound'
]

ROOT_FILES = [
    'sonicr.inf', 'joystick.inf'
]

def pack_assets(src_dir, out_tar_gz):
    print(f"Packing assets from {src_dir} -> {out_tar_gz}...")
    
    with tarfile.open(out_tar_gz, "w:gz") as tar:
        # Top-level directory symlinks: general -> GENERAL, etc.
        for folder in FOLDERS:
            folder_upper = folder.upper()
            src_folder = os.path.join(src_dir, folder)
            if not os.path.exists(src_folder):
                continue
                
            # Add upper directory
            dir_ti = tarfile.TarInfo(name=folder_upper)
            dir_ti.type = tarfile.DIRTYPE
            dir_ti.mode = 0o755
            tar.addfile(dir_ti)
            
            # Add lower directory symlink
            sym_ti = tarfile.TarInfo(name=folder.lower())
            sym_ti.type = tarfile.SYMTYPE
            sym_ti.linkname = folder_upper
            tar.addfile(sym_ti)

            for root, dirs, files in os.walk(src_folder):
                rel = os.path.relpath(root, src_dir)
                rel_parts = rel.split(os.sep)
                rel_upper = "/".join(p.upper() for p in rel_parts)
                
                # Add subdirectories in uppercase
                for d in dirs:
                    sub_upper = f"{rel_upper}/{d.upper()}"
                    sub_ti = tarfile.TarInfo(name=sub_upper)
                    sub_ti.type = tarfile.DIRTYPE
                    sub_ti.mode = 0o755
                    tar.addfile(sub_ti)
                    
                    if d.lower() != d.upper():
                        sub_sym = tarfile.TarInfo(name=f"{rel_upper}/{d.lower()}")
                        sub_sym.type = tarfile.SYMTYPE
                        sub_sym.linkname = d.upper()
                        tar.addfile(sub_sym)
                
                # Add files
                for f in files:
                    full_path = os.path.join(root, f)
                    f_upper = f.upper()
                    arcname_upper = f"{rel_upper}/{f_upper}"
                    
                    # Add uppercase real file
                    tar.add(full_path, arcname=arcname_upper)
                    
                    # Add lowercase symlink if different
                    if f.lower() != f_upper:
                        f_sym = tarfile.TarInfo(name=f"{rel_upper}/{f.lower()}")
                        f_sym.type = tarfile.SYMTYPE
                        f_sym.linkname = f_upper
                        tar.addfile(f_sym)

        # Root files
        for rf in ROOT_FILES:
            full_path = os.path.join(src_dir, rf)
            if os.path.exists(full_path):
                rf_upper = rf.upper()
                tar.add(full_path, arcname=rf_upper)
                if rf.lower() != rf_upper:
                    sym_ti = tarfile.TarInfo(name=rf.lower())
                    sym_ti.type = tarfile.SYMTYPE
                    sym_ti.linkname = rf_upper
                    tar.addfile(sym_ti)

    size_mb = os.path.getsize(out_tar_gz) / (1024 * 1024)
    print(f"Done! Archive created: {out_tar_gz} ({size_mb:.2f} MB)")

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\ssr"
    out = sys.argv[2] if len(sys.argv) > 2 else "sonicr_assets.tar.gz"
    pack_assets(src, out)
