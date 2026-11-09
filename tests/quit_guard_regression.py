#!/usr/bin/env python3
"""Exercise the production GLFW close interception before OF host teardown."""
from pathlib import Path
import os
import subprocess
import tempfile
import attachment_regression as native
from new_preset_regression import block

source = (native.CORE / 'src/ofxOceanode.cpp').read_text()
fixture = r'''
#include <cassert>
#include <functional>
#include <map>
#include <memory>
struct GLFWwindow { int shouldClose=1; };
constexpr int GLFW_FALSE=0;
void glfwSetWindowShouldClose(GLFWwindow* w,int flag){w->shouldClose=flag;}
std::map<GLFWwindow*,std::function<void()>> closeRequests;
PRODUCTION
int main(){
    GLFWwindow window;
    int requests=0,destructions=0;
    closeRequests[&window]=[&](){assert(!window.shouldClose);++requests;};
    guardWindowClose(&window);
    assert(requests==1 && destructions==0 && !window.shouldClose);
    window.shouldClose=1; guardWindowClose(&window);
    assert(requests==2 && !window.shouldClose);
    closeRequests.erase(&window);
    guardWindowClose(&window); assert(requests==2);
}
'''.replace('PRODUCTION', block(source, 'void guardWindowClose('))
with tempfile.TemporaryDirectory(prefix='oceanode-close-guard-') as temp:
    path=Path(temp)
    (path/'check.cpp').write_text(fixture)
    subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-fsanitize=address,undefined',str(path/'check.cpp'),'-o',str(path/'check')],check=True)
    subprocess.run([str(path/'check')],check=True)
print('GLFW close guard passed: close flag canceled before save request, no early exit dispatch.')
