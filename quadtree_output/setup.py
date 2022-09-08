from setuptools import setup, Extension
from torch.utils.cpp_extension import BuildExtension, CppExtension


setup(
    name="quadtree_operations",
    version="2.0.0",
    description="quadtree operations",
    author="Kai Xu",
    ext_modules=[
        CppExtension(
            name="qtree",
            sources=["get_block_corner_list.cpp"],
        )
    ],
    cmdclass={"build_ext": BuildExtension},
)
