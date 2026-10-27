from pathlib import Path

from setuptools import setup, find_packages

# The client ships with the server: its version is the repository VERSION file.
_VERSION_FILE = Path(__file__).resolve().parents[2] / "VERSION"


def _read_version() -> str:
    try:
        version = _VERSION_FILE.read_text(encoding="utf-8").strip()
    except OSError as exc:
        raise RuntimeError(
            f"quantra-client: cannot read the version from {_VERSION_FILE} ({exc}); "
            "build from a full repository checkout"
        ) from exc
    if not version:
        raise RuntimeError(f"quantra-client: {_VERSION_FILE} is empty")
    return version


setup(
    name="quantra-client",
    version=_read_version(),
    description="Python client for Quantra distributed QuantLib pricing engine",
    author="Josep Rupi",
    packages=find_packages(),
    python_requires=">=3.8",
    install_requires=[
        "grpcio>=1.50.0",
        "grpcio-tools>=1.50.0",
        "flatbuffers>=23.0.0",
    ],
    extras_require={
        "dev": [
            "pytest>=7.0.0",
            "pytest-asyncio>=0.20.0",
        ],
    },
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Financial and Insurance Industry",
        "License :: OSI Approved :: BSD License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
    ],
)
