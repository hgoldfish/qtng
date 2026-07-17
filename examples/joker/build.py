#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
joker Alpine/musl 静态构建脚本。

在 Alpine Linux 容器中用 musl 静态链接编译 joker，产出不依赖 glibc、
可在任意同架构 Linux 上运行的可执行文件。

用法:
  ./build.py [--no-cache]
      构建 Docker 镜像并编译 joker-server / joker-client，复制到 dist/
"""

import argparse
import hashlib
import logging
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List

logging.basicConfig(
    level=logging.INFO,
    format="%(levelname)s: %(message)s",
    stream=sys.stderr,
)
logger = logging.getLogger(__name__)

IMAGE_NAME = "joker-musl-static"
DOCKERFILE_HASH_LABEL = "joker.dockerfile_hash"
JOKER_ROOT = Path(__file__).resolve().parent
QTNG_ROOT = JOKER_ROOT.parent.parent
DOCKERFILE_PATH = JOKER_ROOT / "Dockerfile"
DOCKER_BUILD_DIR = JOKER_ROOT / ".build-in-docker"
DIST_DIR = JOKER_ROOT / "dist"
ARTIFACTS = ("joker-server", "joker-client")


def run(cmd: List[str], check: bool = True, quiet: bool = False) -> subprocess.CompletedProcess:
    logger.debug("run: %s", " ".join(cmd))
    kwargs = {"check": check, "universal_newlines": True}
    if quiet:
        kwargs["stdout"] = subprocess.DEVNULL
        kwargs["stderr"] = subprocess.DEVNULL
    return subprocess.run(cmd, **kwargs)


def check_docker() -> None:
    if run(["docker", "--version"], check=False, quiet=True).returncode != 0:
        logger.error("未检测到 Docker，请先安装并启动 Docker")
        sys.exit(1)
    if run(["docker", "info"], check=False, quiet=True).returncode != 0:
        logger.error("Docker 服务未运行")
        sys.exit(1)


def image_exists() -> bool:
    return run(["docker", "image", "inspect", IMAGE_NAME], check=False, quiet=True).returncode == 0


def compute_dockerfile_hash() -> str:
    digest = hashlib.sha256()
    if not DOCKERFILE_PATH.is_file():
        logger.error("找不到构建文件: %s", DOCKERFILE_PATH)
        sys.exit(1)
    digest.update(DOCKERFILE_PATH.read_bytes())
    return digest.hexdigest()


def image_dockerfile_hash() -> str:
    if not image_exists():
        return ""
    result = subprocess.run(
        [
            "docker",
            "image",
            "inspect",
            "--format",
            '{{index .Config.Labels "%s"}}' % DOCKERFILE_HASH_LABEL,
            IMAGE_NAME,
        ],
        check=False,
        universal_newlines=True,
        stdout=subprocess.PIPE,
    )
    if result.returncode != 0:
        return ""
    value = (result.stdout or "").strip()
    if not value or value == "<no value>":
        return ""
    return value


def build_image(no_cache: bool = False) -> None:
    current_hash = compute_dockerfile_hash()
    stored_hash = image_dockerfile_hash()

    if not no_cache and stored_hash == current_hash:
        logger.info(
            "镜像 %s 与 Dockerfile 一致（hash=%s），跳过 docker build",
            IMAGE_NAME,
            current_hash[:12],
        )
        return
    if stored_hash and stored_hash != current_hash:
        logger.info(
            "Dockerfile 已变更（%s -> %s），重建 %s",
            stored_hash[:12],
            current_hash[:12],
            IMAGE_NAME,
        )
    elif not stored_hash and image_exists():
        logger.info("镜像 %s 缺少 hash 标签，重建镜像", IMAGE_NAME)

    cmd = [
        "docker",
        "build",
        "--platform",
        "linux/amd64",
        "--label",
        "%s=%s" % (DOCKERFILE_HASH_LABEL, current_hash),
        "-f",
        str(DOCKERFILE_PATH),
        "-t",
        IMAGE_NAME,
        str(JOKER_ROOT),
    ]
    if no_cache:
        cmd.insert(2, "--no-cache")

    logger.info("构建 Docker 镜像 %s ...", IMAGE_NAME)
    run(cmd)


def install_artifacts() -> None:
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    for name in ARTIFACTS:
        src = DOCKER_BUILD_DIR / name
        if not src.is_file():
            logger.error("未找到编译产出: %s", src)
            sys.exit(1)
        dest = DIST_DIR / name
        shutil.copy2(str(src), str(dest))
        os.chmod(str(dest), 0o755)
        logger.info("copy file: %s -> %s", src, dest)


def build_in_container() -> None:
    if not (QTNG_ROOT / "CMakeLists.txt").is_file():
        logger.error("找不到 qtng 源码树: %s", QTNG_ROOT)
        sys.exit(1)
    cmd = [
        "docker",
        "run",
        "--rm",
        "--platform",
        "linux/amd64",
        "-v",
        "%s:/qtng" % QTNG_ROOT,
        IMAGE_NAME,
    ]
    logger.info("在 Alpine/musl 容器内静态编译，挂载 qtng 为 /qtng ...")
    run(cmd)
    install_artifacts()


def main() -> None:
    parser = argparse.ArgumentParser(description="joker Alpine/musl 静态构建")
    parser.add_argument(
        "--no-cache",
        action="store_true",
        help="强制无缓存重建 Docker 镜像",
    )
    args = parser.parse_args()

    check_docker()
    build_image(no_cache=args.no_cache)
    build_in_container()
    logger.info("完成。静态可执行文件在 %s/", DIST_DIR)


if __name__ == "__main__":
    main()
