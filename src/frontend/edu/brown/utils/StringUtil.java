package edu.brown.utils;

import java.util.Arrays;
import java.util.Collection;

import org.voltdb.client.Client;
import org.voltdb.utils.Pair;

import com.google.protobuf.ByteString;

public abstract class StringUtil {

    public static final String SPACER       = "   ";
    public static final String DOUBLE_LINE  = "============================================================================\n";
    public static final String SINGLE_LINE  = "----------------------------------------------------------------------------\n";

    private static String CACHE_REPEAT_STR = null;
    private static Integer CACHE_REPEAT_SIZE = null;
    private static String CACHE_REPEAT_RESULT = null;
    
    /**
     * Returns the given string repeated the given # of times
     * @param str
     * @param size
     * @return
     */
    public static String repeat(String str, int size) {
        // We cache the last call in case they are making repeated calls for the same thing
        if (CACHE_REPEAT_STR != null &&
            CACHE_REPEAT_STR.equals(str) &&
            CACHE_REPEAT_SIZE.equals(size)) {
            return (CACHE_REPEAT_RESULT);
        }
        
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < size; i++) sb.append(str);
        CACHE_REPEAT_RESULT = sb.toString();
        CACHE_REPEAT_STR = str;
        CACHE_REPEAT_SIZE = size;
        return (CACHE_REPEAT_RESULT);
    }
    
    /**
     * Abbreviate the given string. The last three chars will be periods
     * @param str
     * @param max
     * @return
     */
    public static String abbrv(String str, int max) {
        return (abbrv(str, max, true));
    }

    /**
     * Abbreviate the given string. If dots, then the last three chars will be periods
     * @param str
     * @param max
     * @param dots
     * @return
     */
    public static String abbrv(String str, int max, boolean dots) {
        int len = str.length();
        String ret = null;
        if (len > max) {
            ret = (dots ? str.substring(0, max - 3) + "..." : str.substring(0, max));
        } else {
            ret = str;
        }
        return (ret);
    }
    
    /**
     * Converts a string to title case (ala Python)
     * @param string
     * @return
     */
    public static String title(String string) {
        StringBuilder sb = new StringBuilder();
        String add = "";
        for (String part : string.split(" ")) {
            sb.append(add).append(part.substring(0, 1).toUpperCase());
            if (part.length() > 1) sb.append(part.substring(1).toLowerCase());
            add = " ";
        } // FOR
        return (sb.toString());
    }
    
    /**
     * Append SPACER to the front of each line in a string
     * @param str
     * @return
     */
    public static String addSpacers(String str) {
        StringBuilder sb = new StringBuilder();
        for (String line : str.split("\n")) {
            sb.append(SPACER).append(line).append("\n");
        } // FOR
        return (sb.toString());
    }
    
    /**
     * Python join()
     * @param <T>
     * @param delimiter
     * @param items
     * @return
     */
    public static <T> String join(String delimiter, T...items) {
        return (join(delimiter, Arrays.asList(items)));
    }
    
    /**
     * Python join()
     * @param delimiter
     * @param items
     * @return
     */
    public static String join(String delimiter, Collection<?> items) {
        if (items.isEmpty()) return ("");
     
        StringBuilder sb = new StringBuilder();
        for (Object x : items)
            sb.append(x.toString()).append(delimiter);
        sb.delete(sb.length() - delimiter.length(), sb.length());
     
        return sb.toString();
    }
    
    /**
     * Return the HOST+PORT pair extracted from a string with "<hostname>:<portnum>"
     * @param hostnport
     * @return
     */
    public static Pair<String, Integer> getHostPort(String hostnport) {
        return (getHostPort(hostnport, Client.VOLTDB_SERVER_PORT));
    }
    
    public static Pair<String, Integer> getHostPort(String hostnport, int port) {
        String host = hostnport;
        if (host.contains(":")) {
            String split[] = hostnport.split("\\:", 2);
            host = split[0];
            port = Integer.valueOf(split[1]);
        }
        return (Pair.of(host, port));
    }
    
    private static final char[] CHARACTERS = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    private static char nibbleToHexChar(int nibble) {
        assert 0 <= nibble && nibble < CHARACTERS.length;
        return CHARACTERS[nibble];
    }

    /**
     * Dump a ByteString to a text representation
     * Copied from: http://people.csail.mit.edu/evanj/hg/index.cgi/javatxn/file/tip/src/edu/mit/ExampleServer.java
     * @param bytes
     * @return
     */
    public static StringBuilder hexDump(ByteString bytes) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < bytes.size(); ++i) {
            if (i != 0 && i % 2 == 0) {
                out.append(' ');
            }
            byte b = bytes.byteAt(i);
            out.append(nibbleToHexChar((b >> 4) & 0xf));
            out.append(nibbleToHexChar(b & 0xf));
        }
        return out;
    } 


}
