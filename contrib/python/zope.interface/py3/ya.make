# Generated by devtools/yamaker (pypi).

PY3_LIBRARY()

VERSION(6.2)

LICENSE(ZPL-2.1)

PEERDIR(
    contrib/python/setuptools
)

NO_COMPILER_WARNINGS()

NO_LINT()

SRCS(
    zope/interface/_zope_interface_coptimizations.c
)

PY_REGISTER(
    zope.interface._zope_interface_coptimizations
)

PY_SRCS(
    TOP_LEVEL
    zope/interface/__init__.py
    zope/interface/_compat.py
    zope/interface/_flatten.py
    zope/interface/adapter.py
    zope/interface/advice.py
    zope/interface/common/__init__.py
    zope/interface/common/builtins.py
    zope/interface/common/collections.py
    zope/interface/common/idatetime.py
    zope/interface/common/interfaces.py
    zope/interface/common/io.py
    zope/interface/common/mapping.py
    zope/interface/common/numbers.py
    zope/interface/common/sequence.py
    zope/interface/declarations.py
    zope/interface/document.py
    zope/interface/exceptions.py
    zope/interface/interface.py
    zope/interface/interfaces.py
    zope/interface/registry.py
    zope/interface/ro.py
    zope/interface/verify.py
)

RESOURCE_FILES(
    PREFIX contrib/python/zope.interface/py3/
    .dist-info/METADATA
    .dist-info/top_level.txt
)

END()

RECURSE_FOR_TESTS(
    tests
)
