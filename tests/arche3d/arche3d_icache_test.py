# SPDX-License-Identifier: Apache-2.0
"""Direct cache initialization, then resident hits during delayed refills."""
import gvsoc.runner
import gvsoc.systree as st
from gvsoc.signature import IoV2SingleReq
from vp.clock_domain import Clock_domain


class Board(st.Component):
    def __init__(self, parent, name, parser, options):
        super().__init__(parent, name, options=options)
        clock = Clock_domain(self, 'clock', frequency=1_000_000_000)
        chip = st.Component(self, 'chip')
        clock.o_CLOCK(chip.i_CLOCK())
        cache = st.Component(chip, 'cache')
        cache.add_sources(['pulp/chips/arche3d/logic/icache.cpp'])
        cache.add_properties(dict(size=32768, line_size=64))
        check = st.Component(chip, 'check')
        check.add_sources(['tests/arche3d/icache_checks.cpp'])
        check.itf_bind('fetch', st.SlaveItf(cache, 'input', signature=IoV2SingleReq()),
                       signature=IoV2SingleReq())
        cache.itf_bind('refill', st.SlaveItf(check, 'memory', signature=IoV2SingleReq()),
                       signature=IoV2SingleReq())
        check.itf_bind('flush', st.SlaveItf(cache, 'flush', signature='wire<bool>'),
                       signature='wire<bool>')
        check.itf_bind('preload', st.SlaveItf(cache, 'preload', signature='wire<Arche3dIcachePreload>'),
                       signature='wire<Arche3dIcachePreload>')


class Target(gvsoc.runner.Target):
    def __init__(self, parser, options=None, name=None):
        super().__init__(parser, options, name=name, model=Board,
            description='arche3d instruction cache hit/miss concurrency regression')
