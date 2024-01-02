import cuda_mock
from cuda_mock import *


def test_dynamic_compile_symbol():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    lib = DynamicObj(f'{script_dir}/test_hook_strlen.cxx').appen_compile_opts('-lpthread').compile().get_lib()
    cuda_mock.internal_install_hook("libc.so", "cuda_mock_impl.cpython-38-x86_64-linux-gnu.so", "strlen", str(lib), "strlen")