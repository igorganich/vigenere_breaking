# 1 Basic information
## 1.1 What is this project about?
This project is a tool to crack some cyphertext, encrypted by Vigenere cypher. It contains the necessary source code to build executable file and test suite. 
Based on https://www.cipherchallenge.org/wp-content/uploads/2020/12/Five-ways-to-crack-a-Vigenere-cipher.pdf article.
## 1.2 Vigenere cypher
The Vigenère cipher is a periodic polyalphabetic substitution cipher. The key is a string of characters.
To explain how the cipher works, let's first replace the characters of the key and the characters of the
plaintext by integers, where A=0, B=1, ..., Z=25. The length of the key let's call the period or L. So the
key is just a set of numbers k[0], k[1], ..., k[L-1]. Next take the plaintext and express it also as a list of
numbers: p[0], p[1], p[2], ... The text is encrypted by adding a number from the key modulo 26 to a number
from the plaintext, where we run through the key over and over again as needed as we run through the
plaintext. As an equation, the [i]th character is encrypted like this:
c[i] = (p[i] + k[i mod L]) mod 26
## 1.3 Assumptions
For this project, we assume, that maximum length of encryption key is 12. Also, we assume that plaintext is a string of characters in a lower case with no spaces, quotes, numbers or special characters. And the last - plaintext is some text, written in English. 
# 2 Necessary theory
## 2.1 Frequency tables 
The present method of breaking Vigenere cypher is called **variational method** and it requires information about letter frequencies in natural languages (English in our case) i.e. how often such a letter can be found in some text. To be more precise - we need information about tetragrams (combination of four-letter sequences) frequencies. My source for that is http://practicalcryptography.com/cryptanalysis/letter-frequencies-various-languages/english-letter-frequencies/ and file english_quadgrams.txt. This method is based on the fact that one tetragram in English is more common than another. We'll discuss later how it can be used.
## 2.2 
