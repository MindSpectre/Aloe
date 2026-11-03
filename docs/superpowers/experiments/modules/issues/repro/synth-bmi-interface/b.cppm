module;
#include <string>
export module b;
import a;
export struct B { A a; int g() const { return a.f(); } };
