--TEST--
gzdeflate()/gzinflate()
--SKIPIF--
<?php if (!extension_loaded("zlib")) print "skip"; ?>
--FILE--
<?php /* $Id: 001.phpt,v 1.4 2004/05/19 08:45:23 helly Exp $ */
$original = str_repeat("hallo php",4096);
$packed=gzdeflate($original);
echo strlen($packed)." ".strlen($original)."\n";
$unpacked=gzinflate($packed);
if (strcmp($original,$unpacked)==0) echo "Strings are equal\n";

/* with explicit compression level, length */
$original = str_repeat("hallo php",4096);
$packed=gzdeflate($original, 9);
echo strlen($packed)." ".strlen($original)."\n";
$unpacked=gzinflate($packed, 40000);
if (strcmp($original,$unpacked)==0) echo "Strings are equal\n";

$original = 'aaaaaaaaaaaaaaa';
$packed=gzdeflate($original);
echo strlen($packed)." ".strlen($original)."\n";
$unpacked=gzinflate($packed);
if (strcmp($original,$unpacked)==0) echo "Strings are equal";
?>
--EXPECT--
100 36864
Strings are equal
100 36864
Strings are equal
5 15
Strings are equal