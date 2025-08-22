// wordcount.rs - Word frequency counter
// Build: rustc -O wordcount.rs -o wordcount_rust
// Usage: ./wordcount_rust [filename]

use std::collections::HashMap;
use std::env;
use std::fs::File;
use std::io::{self, Read, Write, BufWriter};
use std::time::Instant;

// FNV-1a hash
#[derive(Default)]
struct FnvHasher {
    state: u64,
}

impl std::hash::Hasher for FnvHasher {
    fn write(&mut self, bytes: &[u8]) {
        let mut hash = if self.state == 0 { 
            14695981039346656037u64 
        } else { 
            self.state 
        };
        for byte in bytes {
            hash ^= *byte as u64;
            hash = hash.wrapping_mul(1099511628211);
        }
        self.state = hash;
    }

    fn finish(&self) -> u64 {
        self.state
    }
}

type FnvBuildHasher = std::hash::BuildHasherDefault<FnvHasher>;
type FnvHashMap<K, V> = HashMap<K, V, FnvBuildHasher>;

fn main() -> io::Result<()> {
    let args: Vec<String> = env::args().collect();
    let filename = args.get(1).map(|s| s.as_str()).unwrap_or("book.txt");
    
    println!("Processing file: {}", filename);
    
    let start_time = Instant::now();
    
    let mut file = match File::open(filename) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("Error: File '{}' not found", filename);
            eprintln!("Error details: {}", e);
            std::process::exit(1);
        }
    };
    
    let mut buffer = Vec::new();
    file.read_to_end(&mut buffer)?;
    
    let file_size = buffer.len() as f64 / 1024.0 / 1024.0;
    
    let mut counts: FnvHashMap<String, u32> = FnvHashMap::with_capacity_and_hasher(
        10_000, 
        FnvBuildHasher::default()
    );
    
    let mut total_words = 0u64;
    let mut current_word = Vec::with_capacity(100);
    
    for &byte in buffer.iter() {
        if byte.is_ascii_alphabetic() {
            current_word.push(byte.to_ascii_lowercase());
        } else if !current_word.is_empty() {
            let word = unsafe { String::from_utf8_unchecked(current_word.clone()) };
            
            *counts.entry(word).or_insert(0) += 1;
            total_words += 1;
            current_word.clear();
        }
    }
    
    if !current_word.is_empty() {
        let word = unsafe { String::from_utf8_unchecked(current_word) };
        *counts.entry(word).or_insert(0) += 1;
        total_words += 1;
    }
    
    let mut sorted: Vec<(&String, &u32)> = counts.iter().collect();
    sorted.sort_unstable_by(|a, b| {
        b.1.cmp(a.1).then_with(|| a.0.cmp(b.0))
    });
    
    let duration = start_time.elapsed();
    let execution_time = duration.as_secs_f64() * 1000.0;
    
    println!("\n=== Top 10 Most Frequent Words ===");
    for (index, (word, count)) in sorted.iter().take(10).enumerate() {
        println!("{:2}. {:<15} {:>8}", index + 1, word, format_number(**count));
    }
    
    println!("\n=== Statistics ===");
    println!("File size:       {:.2} MB", file_size);
    println!("Total words:     {}", format_number(total_words as u32));
    println!("Unique words:    {}", format_number(counts.len() as u32));
    println!("Execution time:  {:.2} ms", execution_time);
    println!("Hash function:   FNV-1a");
    
    write_output_file(filename, &sorted, total_words, counts.len(), execution_time)?;
    
    Ok(())
}

fn format_number(n: u32) -> String {
    let s = n.to_string();
    let mut result = String::new();
    let mut count = 0;
    
    for ch in s.chars().rev() {
        if count == 3 {
            result.push(',');
            count = 0;
        }
        result.push(ch);
        count += 1;
    }
    
    result.chars().rev().collect()
}

fn write_output_file(
    input_filename: &str,
    sorted: &[(&String, &u32)],
    total_words: u64,
    unique_words: usize,
    execution_time: f64,
) -> io::Result<()> {
    let output_filename = input_filename
        .rsplit_once('.')
        .map(|(base, _)| format!("{}_rust_results.txt", base))
        .unwrap_or_else(|| format!("{}_rust_results.txt", input_filename));
    
    let file = File::create(&output_filename)?;
    let mut writer = BufWriter::new(file);
    
    writeln!(writer, "Word Frequency Analysis - Rust Implementation")?;
    writeln!(writer, "Input file: {}", input_filename)?;
    writeln!(writer, "Execution time: {:.2} ms\n", execution_time)?;
    writeln!(writer, "Total words: {}", format_number(total_words as u32))?;
    writeln!(writer, "Unique words: {}\n", format_number(unique_words as u32))?;
    writeln!(writer, "Top 100 Most Frequent Words:")?;
    writeln!(writer, "Rank  Word            Count     Percentage")?;
    writeln!(writer, "----  --------------- --------- ----------")?;
    
    for (index, (word, count)) in sorted.iter().take(100).enumerate() {
        let percentage = (**count as f64 * 100.0) / total_words as f64;
        writeln!(
            writer,
            "{:4}  {:<15} {:>9} {:>10.2}%",
            index + 1,
            word,
            format_number(**count),
            percentage
        )?;
    }
    
    println!("\nResults written to: {}", output_filename);
    Ok(())
}
