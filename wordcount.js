#!/usr/bin/env node
/**
 * Word Frequency Counter - JavaScript (Node.js) Implementation
 * 
 * Optimized for performance while maintaining readability.
 * Best practices applied:
 * - Using Map instead of Object for better performance with large datasets
 * - Pre-compiled regex for efficiency
 * - Using const/let appropriately
 * - Efficient sorting approach
 * 
 * Usage: node wordcount.js [filename]
 * If no filename provided, defaults to 'book.txt'
 */

const fs = require('fs');
const { performance } = require('perf_hooks');

// Get filename from command line or use default
const filename = process.argv[2] || 'book.txt';

// Check if file exists
if (!fs.existsSync(filename)) {
    console.error(`Error: File '${filename}' not found`);
    console.log('Usage: node wordcount.js [filename]');
    console.log('\nTo create a test file:');
    console.log('curl https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt');
    process.exit(1);
}

console.log(`Processing file: ${filename}`);
const startTime = performance.now();
const startMemory = process.memoryUsage().heapUsed;

try {
    // Read entire file at once (fastest for files that fit in memory)
    const text = fs.readFileSync(filename, 'utf8');
    
    // Convert to lowercase for case-insensitive counting
    const lowerText = text.toLowerCase();
    
    // Pre-compiled regex for word extraction (more efficient than creating it each time)
    // Matches word boundaries with letters only
    const wordRegex = /\b[a-z]+\b/g;
    
    // Extract all words using regex
    const words = lowerText.match(wordRegex);
    
    if (!words) {
        console.log('No words found in file');
        process.exit(0);
    }
    
    // Use Map for better performance with many keys
    const counts = new Map();
    
    // Count word frequencies
    for (const word of words) {
        counts.set(word, (counts.get(word) || 0) + 1);
    }
    
    // Convert to array and sort by count (descending)
    const sorted = Array.from(counts.entries())
        .sort((a, b) => b[1] - a[1]);
    
    // Calculate statistics
    const endTime = performance.now();
    const endMemory = process.memoryUsage().heapUsed;
    const executionTime = (endTime - startTime).toFixed(2);
    const memoryUsed = ((endMemory - startMemory) / 1024 / 1024).toFixed(2);
    const fileSize = (fs.statSync(filename).size / 1024 / 1024).toFixed(2);
    
    // Output results
    console.log('\n=== Top 10 Most Frequent Words ===');
    sorted.slice(0, 10).forEach(([word, count], index) => {
        console.log(`${(index + 1).toString().padStart(2)}. ${word.padEnd(15)} ${count.toLocaleString()}`);
    });
    
    console.log('\n=== Statistics ===');
    console.log(`File size:       ${fileSize} MB`);
    console.log(`Total words:     ${words.length.toLocaleString()}`);
    console.log(`Unique words:    ${counts.size.toLocaleString()}`);
    console.log(`Execution time:  ${executionTime} ms`);
    console.log(`Memory used:     ${memoryUsed} MB`);
    console.log(`Node.js version: ${process.version}`);
    
    // Write results to output file
    const outputFilename = filename.replace(/\.[^.]+$/, '') + '_javascript_results.txt';
    const outputContent = [
        'Word Frequency Analysis - JavaScript Implementation',
        `Input file: ${filename}`,
        `Generated: ${new Date().toISOString()}`,
        `Execution time: ${executionTime} ms`,
        '',
        `Total words: ${words.length.toLocaleString()}`,
        `Unique words: ${counts.size.toLocaleString()}`,
        '',
        'Top 100 Most Frequent Words:',
        'Rank  Word            Count     Percentage',
        '----  --------------- --------- ----------',
        ...sorted.slice(0, 100).map(([word, count], index) => {
            const percentage = (count * 100 / words.length).toFixed(2);
            return `${(index + 1).toString().padStart(4)}  ${word.padEnd(15)} ${count.toString().padStart(9)} ${percentage.padStart(10)}%`;
        })
    ].join('\n');
    
    fs.writeFileSync(outputFilename, outputContent);
    console.log(`\nResults written to: ${outputFilename}`);
    
} catch (error) {
    console.error('Error processing file:', error.message);
    process.exit(1);
}

/**
 * Performance notes:
 * - Map is faster than Object for frequent get/set operations
 * - Pre-compiled regex avoids re-compilation overhead
 * - Reading entire file is faster than streaming for files < 100MB
 * - Array.from() with sort is efficient for final sorting
 * 
 * For even larger files (> 100MB), consider:
 * - Using readline or stream processing
 * - Worker threads for parallel processing
 * - Native addons for critical sections
 */