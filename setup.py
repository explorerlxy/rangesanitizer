#!/usr/bin/env python3
import sys
import os.path
curdir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(curdir, 'infra'))
import infra
from infra.context import Context




if __name__ == '__main__':
    setup = infra.Setup(__file__)

    setup.add_target(infra.targets.SPEC2006(
        patches = ['asan', 'rsan'],
        source=os.path.join(curdir, 'spec2006'),
        source_type='installed'
    ))

    setup.add_target(infra.targets.Juliet())

    setup.main()