<?php
use App\Services\Foo as Bar;
class Foo { public static function make() {} public static function o(): Other {}
  public static function me(): static {} }
class Other {}
function f()
{
    $a = new Bar();
    $a->
    $b = Foo::make();
    $b->
    $c = Foo::o();
    $c->
    $e = \App\Foo::me();
    $e->
    $g = Foo::BAR;
    $g->
}
