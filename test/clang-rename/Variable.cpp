// RUN: cat %s > %t.cpp
// RUN: clang-rename -offset=148 -new-name=Bar %t.cpp -i --
// RUN: sed 's,//.*,,' %t.cpp | FileCheck %s

namespace A {
int Foo;                // CHECK: int Bar;
}
int Foo;                // CHECK: int Foo;
int Qux = Foo;          // CHECK: int Qux = Foo;
int Baz = A::Foo;       // CHECK: Baz = A::Bar;
void fun() {
  struct {
    int Foo;            // CHECK: int Foo;
  } b = {100};
  int Foo = 100;        // CHECK: int Foo = 100;
  Baz = Foo;            // CHECK: Baz = Foo;
  {
    extern int Foo;     // CHECK: extern int Foo;
    Baz = Foo;          // CHECK: Baz = Foo;
    Foo = A::Foo + Baz; // CHECK: Foo = A::Bar + Baz;
    A::Foo = b.Foo;     // CHECK: A::Bar = b.Foo;
  }
  Foo = b.Foo;          // Foo = b.Foo;
}

// Use grep -FUbo 'Foo' <file> to get the correct offset of foo when changing
// this file.
