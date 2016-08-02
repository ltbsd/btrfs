#include "btrfs_drv.h"

#define Z_SOLO
#define ZLIB_INTERNAL

#include "zlib/zlib.h"
#include "zlib/inftrees.h"
#include "zlib/inflate.h"

static void* zlib_alloc(void* opaque, unsigned int items, unsigned int size) {
    return ExAllocatePoolWithTag(PagedPool, items * size, ALLOC_TAG_ZLIB);
}

static void zlib_free(void* opaque, void* ptr) {
    ExFreePool(ptr);
}

NTSTATUS decompress(UINT8 type, UINT8* inbuf, UINT64 inlen, UINT8* outbuf, UINT64 outlen) {
    z_stream c_stream;
    int ret;

    if (type != BTRFS_COMPRESSION_ZLIB) {
        ERR("unsupported compression type %x\n", type);
        return STATUS_NOT_SUPPORTED;
    }

    c_stream.zalloc = zlib_alloc;
    c_stream.zfree = zlib_free;
    c_stream.opaque = (voidpf)0;

    ret = inflateInit(&c_stream);
    
    if (ret != Z_OK) {
        ERR("inflateInit returned %08x\n", ret);
        return STATUS_INTERNAL_ERROR;
    }

    c_stream.next_in = inbuf;
    c_stream.avail_in = inlen;
    
    c_stream.next_out = outbuf;
    c_stream.avail_out = outlen;
    
    do {
        ret = inflate(&c_stream, Z_NO_FLUSH);
        
        if (ret != Z_OK && ret != Z_STREAM_END) {
            ERR("inflate returned %08x\n", ret);
            inflateEnd(&c_stream);
            return STATUS_INTERNAL_ERROR;
        }
    } while (ret != Z_STREAM_END);

    ret = inflateEnd(&c_stream);
    
    if (ret != Z_OK) {
        ERR("inflateEnd returned %08x\n", ret);
        return STATUS_INTERNAL_ERROR;
    }
    
    // FIXME - if we're short, should we zero the end of outbuf so we don't leak information into userspace?
    
    return STATUS_SUCCESS;
}

NTSTATUS write_compressed_bit(fcb* fcb, UINT64 start_data, UINT64 end_data, void* data, LIST_ENTRY* changed_sector_list, PIRP Irp, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    UINT8 compression;
    UINT64 comp_length;
    UINT8* comp_data;
    UINT32 out_left;
    LIST_ENTRY* le;
    chunk* c;
    z_stream c_stream;
    int ret;
    
    comp_data = ExAllocatePoolWithTag(PagedPool, end_data - start_data, ALLOC_TAG);
    if (!comp_data) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    Status = excise_extents(fcb->Vcb, fcb, start_data, end_data, rollback);
    if (!NT_SUCCESS(Status)) {
        ERR("excise_extents returned %08x\n", Status);
        ExFreePool(comp_data);
        return Status;
    }
    
    c_stream.zalloc = zlib_alloc;
    c_stream.zfree = zlib_free;
    c_stream.opaque = (voidpf)0;

    ret = deflateInit(&c_stream, 3);
    
    if (ret != Z_OK) {
        ERR("deflateInit returned %08x\n", ret);
        ExFreePool(comp_data);
        return STATUS_INTERNAL_ERROR;
    }
    
    c_stream.avail_in = end_data - start_data;
    c_stream.next_in = data;
    c_stream.avail_out = end_data - start_data;
    c_stream.next_out = comp_data;
    
    do {
        ret = deflate(&c_stream, Z_FINISH);
        
        if (ret == Z_STREAM_ERROR) {
            ERR("deflate returned %x\n", ret);
            ExFreePool(comp_data);
            return STATUS_INTERNAL_ERROR;
        }
    } while (c_stream.avail_in > 0 && c_stream.avail_out > 0);
    
    out_left = c_stream.avail_out;
    
    ret = deflateEnd(&c_stream);
    
    if (ret != Z_OK) {
        ERR("deflateEnd returned %08x\n", ret);
        ExFreePool(comp_data);
        return STATUS_INTERNAL_ERROR;
    }
    
    if (out_left < fcb->Vcb->superblock.sector_size) { // compressed extent would be larger than or same size as uncompressed extent
        ExFreePool(comp_data);
        
        comp_length = end_data - start_data;
        comp_data = data;
        compression = BTRFS_COMPRESSION_NONE;
    } else {
        UINT32 cl;
        
        compression = BTRFS_COMPRESSION_ZLIB;
        cl = end_data - start_data - out_left;
        comp_length = sector_align(cl, fcb->Vcb->superblock.sector_size);
        
        RtlZeroMemory(comp_data + cl, comp_length - cl);
    }
    
    ExAcquireResourceExclusiveLite(&fcb->Vcb->chunk_lock, TRUE);
    
    le = fcb->Vcb->chunks.Flink;
    while (le != &fcb->Vcb->chunks) {
        c = CONTAINING_RECORD(le, chunk, list_entry);
        
        ExAcquireResourceExclusiveLite(&c->nonpaged->lock, TRUE);
        
        if (c->chunk_item->type == fcb->Vcb->data_flags && (c->chunk_item->size - c->used) >= comp_length) {
            if (insert_extent_chunk(fcb->Vcb, fcb, c, start_data, comp_length, FALSE, comp_data, changed_sector_list, Irp, rollback, compression, end_data - start_data)) {
                ExReleaseResourceLite(&c->nonpaged->lock);
                ExReleaseResourceLite(&fcb->Vcb->chunk_lock);
                
                if (compression != BTRFS_COMPRESSION_NONE)
                    ExFreePool(comp_data);
                
                return STATUS_SUCCESS;
            }
        }
        
        ExReleaseResourceLite(&c->nonpaged->lock);

        le = le->Flink;
    }
    
    if ((c = alloc_chunk(fcb->Vcb, fcb->Vcb->data_flags, rollback))) {
        ExAcquireResourceExclusiveLite(&c->nonpaged->lock, TRUE);
        
        if (c->chunk_item->type == fcb->Vcb->data_flags && (c->chunk_item->size - c->used) >= comp_length) {
            if (insert_extent_chunk(fcb->Vcb, fcb, c, start_data, comp_length, FALSE, comp_data, changed_sector_list, Irp, rollback, compression, end_data - start_data)) {
                ExReleaseResourceLite(&c->nonpaged->lock);
                ExReleaseResourceLite(&fcb->Vcb->chunk_lock);
                
                if (compression != BTRFS_COMPRESSION_NONE)
                    ExFreePool(comp_data);
                
                return STATUS_SUCCESS;
            }
        }
        
        ExReleaseResourceLite(&c->nonpaged->lock);
    }
    
    ExReleaseResourceLite(&fcb->Vcb->chunk_lock);
    
    WARN("couldn't find any data chunks with %llx bytes free\n", comp_length);

    return STATUS_DISK_FULL;
}
