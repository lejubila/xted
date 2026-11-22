<?php
/**
 * @method static Foo make()
 */
class Thing extends Base
{
    const MAX = 3;
    public $level = 1;
    public function run(int $n)
    {
        $this->extra = 1;
    }
}
function helper($a, $b) {}
