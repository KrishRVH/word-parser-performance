#!/usr/bin/env php
<?php
// wordcount.php - Word frequency counter
// Usage: php wordcount.php [filename]

$filename = $argv[1] ?? 'book.txt';

if (!file_exists($filename)) {
    fwrite(STDERR, "Error: File '$filename' not found\n");
    echo "Usage: php wordcount.php [filename]\n\n";
    echo "To create a test file:\n";
    echo "curl https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt\n";
    exit(1);
}

echo "Processing file: $filename\n";

$startTime = microtime(true);
$startMemory = memory_get_usage(true);

try {
    $text = file_get_contents($filename);
    
    if ($text === false) {
        throw new Exception("Failed to read file");
    }
    
    $text = strtolower($text);
    
    preg_match_all('/\b[a-z]+\b/', $text, $matches);
    
    if (empty($matches[0])) {
        echo "No words found in file\n";
        exit(0);
    }
    
    $counts = array_count_values($matches[0]);
    
    arsort($counts);
    
    $endTime = microtime(true);
    $endMemory = memory_get_usage(true);
    $executionTime = number_format(($endTime - $startTime) * 1000, 2);
    $memoryUsed = number_format(($endMemory - $startMemory) / 1024 / 1024, 2);
    $fileSize = number_format(filesize($filename) / 1024 / 1024, 2);
    $totalWords = count($matches[0]);
    $uniqueWords = count($counts);
    
    echo "\n=== Top 10 Most Frequent Words ===\n";
    $top10 = array_slice($counts, 0, 10, true);
    $index = 1;
    foreach ($top10 as $word => $count) {
        printf("%2d. %-15s %s\n", 
            $index++, 
            $word, 
            number_format($count)
        );
    }
    
    echo "\n=== Statistics ===\n";
    echo "File size:       $fileSize MB\n";
    echo "Total words:     " . number_format($totalWords) . "\n";
    echo "Unique words:    " . number_format($uniqueWords) . "\n";
    echo "Execution time:  $executionTime ms\n";
    echo "Memory used:     $memoryUsed MB\n";
    echo "PHP version:     " . PHP_VERSION . "\n";
    
    if (!ini_get('opcache.enable_cli')) {
        echo "\nTip: Enable OPcache CLI for better performance:\n";
        echo "     php -d opcache.enable_cli=1 wordcount.php\n";
    }
    
    $outputFilename = preg_replace('/\.[^.]+$/', '', $filename) . '_php_results.txt';
    $output = fopen($outputFilename, 'w');
    if ($output) {
        fwrite($output, "Word Frequency Analysis - PHP Implementation\n");
        fwrite($output, "Input file: $filename\n");
        fwrite($output, "Generated: " . date('Y-m-d H:i:s') . "\n");
        fwrite($output, "Execution time: $executionTime ms\n\n");
        fwrite($output, "Total words: " . number_format($totalWords) . "\n");
        fwrite($output, "Unique words: " . number_format($uniqueWords) . "\n\n");
        fwrite($output, "Top 100 Most Frequent Words:\n");
        fwrite($output, "Rank  Word            Count     Percentage\n");
        fwrite($output, "----  --------------- --------- ----------\n");
        
        $top100 = array_slice($counts, 0, 100, true);
        $index = 1;
        foreach ($top100 as $word => $count) {
            $percentage = ($count * 100 / $totalWords);
            fprintf($output, "%4d  %-15s %9s %10.2f%%\n", 
                $index++, 
                $word, 
                number_format($count),
                $percentage
            );
        }
        
        fclose($output);
        echo "\nResults written to: $outputFilename\n";
    }
    
} catch (Exception $e) {
    fwrite(STDERR, "Error processing file: " . $e->getMessage() . "\n");
    exit(1);
}