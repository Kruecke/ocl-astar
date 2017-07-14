# ocl-astar
OpenCL A* Implementations

## Windows
For AMD: Download and install OpenCL SDK from [Github](https://github.com/GPUOpen-LibrariesAndSDKs/OCL-SDK/releases).

Download [boost](http://www.boost.org/) an extract it to a subfolder called `boost`.

For some reason, Compute wants to make use of `libboost_chrono` so let's just build all the libraries.
Open VS command prompt, switch into the boost directory and run `bootstrap` and `b2`.
