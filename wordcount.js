#!/usr/bin/env node
// wordcount.js - Word frequency counter
// Usage: node wordcount.js [filename]

const fs = require('fs');
const { performance } = require('perf_hooks');

const filename = process.argv[2] || 'book.txt';

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
    const stats = fs.statSync(filename);
    const buffer = Buffer.allocUnsafe(stats.size);
    const fd = fs.openSync(filename, 'r');
    fs.readSync(fd, buffer, 0, stats.size, 0);
    fs.closeSync(fd);
    
    const counts = new Map();
    let currentWord = '';
    let totalWords = 0;
    
    const isLetter = new Uint8Array(256);
    for (let i = 65; i <= 90; i++) isLetter[i] = 1;
    for (let i = 97; i <= 122; i++) isLetter[i] = 1;
    
    for (let i = 0; i < buffer.length; i++) {
        const char = buffer[i];
        
        if (isLetter[char]) {
            currentWord += String.fromCharCode(char >= 65 && char <= 90 ? char + 32 : char);
        } else if (currentWord.length > 0) {
            const count = counts.get(currentWord);
            counts.set(currentWord, count ? count + 1 : 1);
            totalWords++;
            currentWord = '';
        }
    }
    
    if (currentWord.length > 0) {
        const count = counts.get(currentWord);
        counts.set(currentWord, count ? count + 1 : 1);
        totalWords++;
    }
    
    // Use typed array for sorting to avoid object allocation
    const sorted = Array.from(counts.entries())
        .sort((a, b) => b[1] - a[1]);
    
    const endTime = performance.now();
    const endMemory = process.memoryUsage().heapUsed;
    const executionTime = (endTime - startTime).toFixed(2);
    const memoryUsed = ((endMemory - startMemory) / 1024 / 1024).toFixed(2);
    const fileSize = (stats.size / 1024 / 1024).toFixed(2);
    
    console.log('\n=== Top 10 Most Frequent Words ===');
    sorted.slice(0, 10).forEach(([word, count], index) => {
        console.log(`${(index + 1).toString().padStart(2)}. ${word.padEnd(15)} ${count.toLocaleString()}`);
    });
    
    console.log('\n=== Statistics ===');
    console.log(`File size:       ${fileSize} MB`);
    console.log(`Total words:     ${totalWords.toLocaleString()}`);
    console.log(`Unique words:    ${counts.size.toLocaleString()}`);
    console.log(`Execution time:  ${executionTime} ms`);
    console.log(`Memory used:     ${memoryUsed} MB`);
    console.log(`Node.js version: ${process.version}`);
    
    const outputFilename = filename.replace(/\.[^.]+$/, '') + '_javascript_results.txt';
    const outputContent = [
        'Word Frequency Analysis - JavaScript Implementation',
        `Input file: ${filename}`,
        `Generated: ${new Date().toISOString()}`,
        `Execution time: ${executionTime} ms`,
        '',
        `Total words: ${totalWords.toLocaleString()}`,
        `Unique words: ${counts.size.toLocaleString()}`,
        '',
        'Top 100 Most Frequent Words:',
        'Rank  Word            Count     Percentage',
        '----  --------------- --------- ----------',
        ...sorted.slice(0, 100).map(([word, count], index) => {
            const percentage = (count * 100 / totalWords).toFixed(2);
            return `${(index + 1).toString().padStart(4)}  ${word.padEnd(15)} ${count.toString().padStart(9)} ${percentage.padStart(10)}%`;
        })
    ].join('\n');
    
    fs.writeFileSync(outputFilename, outputContent);
    console.log(`\nResults written to: ${outputFilename}`);
    
} catch (error) {
    console.error('Error processing file:', error.message);
    process.exit(1);
}