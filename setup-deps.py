import os
import sys
import platform
import hashlib
import urllib.request
import zipfile
import shutil

ORGANIZATION = "moonlight-stream"
PREBUILT_REPO = "moonlight-qt-deps"
TAG = "v12"
PLATFORM_CONFIG = {
    "Darwin": (
        "mac",
        "macOS-universal.zip",
        "446228fb68d5aef4bb8791f08e5e98e38e6ecffdefac7633902fc463189a0f3c",
    ),
    "Linux": (
        "steamlink",
        "steamlink.zip",
        "c4d58d8e8ffee46531cf9a93d9daf3fbc81f831685bba55c4222e995764025bf",
    ),
}

def get_platform_config():
    system = platform.system()
    if system in PLATFORM_CONFIG:
        return PLATFORM_CONFIG[system]

    print(f"Error: Unsupported platform ({system})")
    sys.exit(1)

def verify_sha256(path, expected_hash):
    digest = hashlib.sha256()
    with open(path, "rb") as archive:
        for chunk in iter(lambda: archive.read(1024 * 1024), b""):
            digest.update(chunk)

    actual_hash = digest.hexdigest()
    if actual_hash != expected_hash:
        raise RuntimeError(
            f"SHA256 mismatch for {path}. "
            f"Expected {expected_hash} but got {actual_hash}."
        )

def download_and_extract():
    subfolder, asset_name, sha256 = get_platform_config()
    
    target_dir = os.path.join(os.getcwd(), "libs", subfolder)
    url = f"https://github.com/{ORGANIZATION}/{PREBUILT_REPO}/releases/download/{TAG}/{asset_name}"

    if os.path.exists(target_dir):
        print("Cleaning target directory...")
        shutil.rmtree(target_dir)

    os.makedirs(target_dir, exist_ok=True)

    archive_path = os.path.join(target_dir, asset_name)

    print(f"Downloading {asset_name}...")
    try:
        urllib.request.urlretrieve(url, archive_path)
    except Exception as e:
        print(f"Download failed: {e}")
        sys.exit(1)

    print(f"Verifying {asset_name}...")
    verify_sha256(archive_path, sha256)

    print(f"Extracting {asset_name}...")
    with zipfile.ZipFile(archive_path, 'r') as zip_ref:
        zip_ref.extractall(target_dir)

    os.remove(archive_path)
    print(f"Dependencies successfully deployed")

if __name__ == "__main__":
    download_and_extract()
