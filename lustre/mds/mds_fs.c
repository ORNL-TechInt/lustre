/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  linux/mds/mds_fs.c
 *
 *  Lustre Metadata Server (MDS) filesystem interface code
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *
 *  This code is issued under the GNU General Public License.
 *  See the file COPYING in this distribution
 *
 *  by Andreas Dilger <adilger@clusterfs.com>
 *
 */

#define EXPORT_SYMTAB
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/lustre_mds.h>

LIST_HEAD(mds_fs_types);

struct mds_fs_type {
        struct list_head                 mft_list;
        struct mds_fs_operations        *mft_ops;
        char                            *mft_name;
};

/* This will be a hash table at some point. */
static int mds_init_client_data(struct mds_obd *mds)
{
        INIT_LIST_HEAD(&mds->mds_client_info);
        return 0;
}

#define MDS_MAX_CLIENTS 1024
#define MDS_MAX_CLIENT_WORDS (MDS_MAX_CLIENTS / sizeof(unsigned long))

static unsigned long last_rcvd_slots[MDS_MAX_CLIENT_WORDS];

/* Add client data to the MDS.  The in-memory storage will be a hash at some
 * point.  We use a bitmap to locate a free space in the last_rcvd file if
 * cl_off is -1 (i.e. a new client).  Otherwise, we have just read the data
 * from the last_rcvd file and we know its offset.
 */
int mds_client_add(struct mds_obd *mds, struct mds_client_data *mcd, int cl_off)
{
        struct mds_client_info *mci;

        OBD_ALLOC(mci, sizeof(*mci));
        if (!mci) {
                CERROR("no memory for MDS client info\n");
                RETURN(-ENOMEM);
        }
        INIT_LIST_HEAD(&mci->mci_open_head);

        CDEBUG(D_INFO, "client at offset %d with UUID '%s' added\n",
               cl_off, mcd->mcd_uuid);

        if (cl_off == -1) {
                unsigned long *word;
                int bit;

        repeat:
                word = last_rcvd_slots;
                while(*word == ~0UL)
                        ++word;
                if (word - last_rcvd_slots >= MDS_MAX_CLIENT_WORDS) {
                        CERROR("no room in client MDS bitmap - fix code\n");
                        return -ENOMEM;
                }
                bit = ffz(*word);
                if (test_and_set_bit(bit, word)) {
                        CERROR("found bit %d set for word %d - fix code\n",
                               bit, word - last_rcvd_slots);
                        goto repeat;
                }
                cl_off = word - last_rcvd_slots + bit;
        } else {
                if (test_and_set_bit(cl_off, last_rcvd_slots)) {
                        CERROR("bit %d already set in bitmap - bad bad\n",
                               cl_off);
                        LBUG();
                }
        }

        mci->mci_mcd = mcd;
        mci->mci_off = cl_off;

        /* For now we just put the clients in a list, not a hashed list */
        list_add_tail(&mci->mci_list, &mds->mds_client_info);

        mds->mds_client_count++;

        return 0;
}

void mds_client_del(struct mds_obd *mds, struct mds_client_info *mci)
{
        unsigned long *word;
        int bit;

        word = last_rcvd_slots + mci->mci_off / sizeof(unsigned long);
        bit = mci->mci_off % sizeof(unsigned long);

        if (!test_and_clear_bit(bit, word)) {
                CERROR("bit %d already clear in word %d - bad bad\n",
                       bit, word - last_rcvd_slots);
                LBUG();
        }

        --mds->mds_client_count;
        list_del(&mci->mci_list);
        OBD_FREE(mci->mci_mcd, sizeof(*mci->mci_mcd));
        OBD_FREE(mci, sizeof (*mci));
}

static int mds_client_free_all(struct mds_obd *mds)
{
        struct list_head *p, *n;

        list_for_each_safe(p, n, &mds->mds_client_info) {
                struct mds_client_info *mci;

                mci = list_entry(p, struct mds_client_info, mci_list);
                mds_client_del(mds, mci);
        }

        return 0;
}

static int mds_server_free_data(struct mds_obd *mds)
{
        OBD_FREE(mds->mds_server_data, sizeof(*mds->mds_server_data));
        mds->mds_server_data = NULL;

        return 0;
}

#define LAST_RCVD "last_rcvd"

static int mds_read_last_rcvd(struct mds_obd *mds, struct file *f)
{
        struct mds_server_data *msd;
        struct mds_client_data *mcd = NULL;
        loff_t fsize = f->f_dentry->d_inode->i_size;
        loff_t off = 0;
        int cl_off;
        __u64 last_rcvd = 0;
        __u64 last_mount;
        int rc = 0;

        OBD_ALLOC(msd, sizeof(*msd));
        if (!msd)
                RETURN(-ENOMEM);
        rc = lustre_fread(f, (char *)msd, sizeof(*msd), &off);

        mds->mds_server_data = msd;
        if (rc == 0) {
                CERROR("empty MDS %s, new MDS?\n", LAST_RCVD);
                RETURN(0);
        } else if (rc != sizeof(*msd)) {
                CERROR("error reading MDS %s: rc = %d\n", LAST_RCVD, rc);
                if (rc > 0) {
                        rc = -EIO;
                }
                GOTO(err_msd, rc);
        }

        /*
         * When we do a clean MDS shutdown, we save the last_rcvd into
         * the header.  If we find clients with higher last_rcvd values
         * then those clients may need recovery done.
         */
        last_rcvd = le64_to_cpu(msd->msd_last_rcvd);
        mds->mds_last_rcvd = last_rcvd;
        CDEBUG(D_INODE, "got %Lu for server last_rcvd value\n",
               (unsigned long long)last_rcvd);

        last_mount = le64_to_cpu(msd->msd_mount_count);
        mds->mds_mount_count = last_mount;
        CDEBUG(D_INODE, "got %Lu for server last_mount value\n",
               (unsigned long long)last_mount);

        for (off = MDS_LR_CLIENT, cl_off = 0, rc = sizeof(*mcd);
             off <= fsize - sizeof(*mcd) && rc == sizeof(*mcd);
             off = MDS_LR_CLIENT + ++cl_off * MDS_LR_SIZE) {
                if (!mcd)
                        OBD_ALLOC(mcd, sizeof(*mcd));
                if (!mcd)
                        GOTO(err_msd, rc = -ENOMEM);

                rc = lustre_fread(f, (char *)mcd, sizeof(*mcd), &off);
                if (rc != sizeof(*mcd)) {
                        CERROR("error reading MDS %s offset %d: rc = %d\n",
                               LAST_RCVD, cl_off, rc);
                        if (rc > 0)
                                rc = -EIO;
                        break;
                }

                last_rcvd = le64_to_cpu(mcd->mcd_last_rcvd);
                last_mount = le64_to_cpu(mcd->mcd_mount_count);

                if (last_rcvd &&
                    last_mount - mcd->mcd_mount_count < MDS_MOUNT_RECOV) {
                        rc = mds_client_add(mds, mcd, cl_off);
                        if (rc) {
                                rc = 0;
                                break;
                        }
                        mcd = NULL;
                } else {
                        CDEBUG(D_INFO,
                               "client at offset %d with UUID '%s' ignored\n",
                               cl_off, mcd->mcd_uuid);
                }

                if (last_rcvd > mds->mds_last_rcvd) {
                        CDEBUG(D_OTHER,
                               "client at offset %d has last_rcvd = %Lu\n",
                               cl_off, (unsigned long long)last_rcvd);
                        mds->mds_last_rcvd = last_rcvd;
                }
        }
        CDEBUG(D_INODE, "got %Lu for highest last_rcvd value, %d clients\n",
               (unsigned long long)mds->mds_last_rcvd, mds->mds_client_count);

        /* After recovery, there can be no local uncommitted transactions */
        mds->mds_last_committed = mds->mds_last_rcvd;

        return 0;

err_msd:
        mds_server_free_data(mds);
        return rc;
}

static int mds_fs_prep(struct mds_obd *mds)
{
        struct obd_run_ctxt saved;
        struct dentry *dentry;
        struct file *f;
        int rc;

        push_ctxt(&saved, &mds->mds_ctxt);
        dentry = simple_mkdir(current->fs->pwd, "ROOT", 0755);
        if (IS_ERR(dentry)) {
                rc = PTR_ERR(dentry);
                CERROR("cannot create ROOT directory: rc = %d\n", rc);
                GOTO(err_pop, rc);
        }
        /* XXX probably want to hold on to this later... */
        dput(dentry);
        f = filp_open("ROOT", O_RDONLY, 0);
        if (IS_ERR(f)) {
                rc = PTR_ERR(f);
                CERROR("cannot open ROOT: rc = %d\n", rc);
                LBUG();
                GOTO(err_pop, rc);
        }

        mds->mds_rootfid.id = f->f_dentry->d_inode->i_ino;
        mds->mds_rootfid.generation = f->f_dentry->d_inode->i_generation;
        mds->mds_rootfid.f_type = S_IFDIR;

        rc = filp_close(f, 0);
        if (rc) {
                CERROR("cannot close ROOT: rc = %d\n", rc);
                LBUG();
        }

        dentry = simple_mkdir(current->fs->pwd, "FH", 0700);
        if (IS_ERR(dentry)) {
                rc = PTR_ERR(dentry);
                CERROR("cannot create FH directory: rc = %d\n", rc);
                GOTO(err_pop, rc);
        }
        /* XXX probably want to hold on to this later... */
        dput(dentry);

        rc = mds_init_client_data(mds);
        if (rc)
                GOTO(err_pop, rc);

        f = filp_open(LAST_RCVD, O_RDWR | O_CREAT, 0644);
        if (IS_ERR(f)) {
                rc = PTR_ERR(f);
                CERROR("cannot open/create %s file: rc = %d\n", LAST_RCVD, rc);
                GOTO(err_pop, rc = PTR_ERR(f));
        }
        if (!S_ISREG(f->f_dentry->d_inode->i_mode)) {
                CERROR("%s is not a regular file!: mode = %o\n", LAST_RCVD,
                       f->f_dentry->d_inode->i_mode);
                GOTO(err_pop, rc = -ENOENT);
        }

        rc = mds_fs_journal_data(mds, f);
        if (rc) {
                CERROR("cannot journal data on %s: rc = %d\n", LAST_RCVD, rc);
                GOTO(err_filp, rc);
        }

        rc = mds_read_last_rcvd(mds, f);
        if (rc) {
                CERROR("cannot read %s: rc = %d\n", LAST_RCVD, rc);
                GOTO(err_client, rc);
        }
        mds->mds_rcvd_filp = f;
        pop_ctxt(&saved);

        RETURN(0);

err_client:
        mds_client_free_all(mds);
err_filp:
        if (filp_close(f, 0))
                CERROR("can't close %s after error\n", LAST_RCVD);
err_pop:
        pop_ctxt(&saved);

        return rc;
}

static struct mds_fs_operations *mds_search_fs_type(const char *name)
{
        struct list_head *p;
        struct mds_fs_type *type;

        /* lock mds_fs_types list */
        list_for_each(p, &mds_fs_types) {
                type = list_entry(p, struct mds_fs_type, mft_list);
                if (!strcmp(type->mft_name, name)) {
                        /* unlock mds_fs_types list */
                        return type->mft_ops;
                }
        }
        /* unlock mds_fs_types list */
        return NULL;
}

int mds_register_fs_type(struct mds_fs_operations *ops, const char *name)
{
        struct mds_fs_operations *found;
        struct mds_fs_type *type;

        if ((found = mds_search_fs_type(name))) {
                if (found != ops) {
                        CERROR("different operations for type %s\n", name);
                        RETURN(-EEXIST);
                }
                return 0;
        }
        OBD_ALLOC(type, sizeof(*type));
        if (!type)
                RETURN(-ENOMEM);

        INIT_LIST_HEAD(&type->mft_list);
        type->mft_ops = ops;
        type->mft_name = strdup(name);
        if (!type->mft_name) {
                OBD_FREE(type, sizeof(*type));
                RETURN(-ENOMEM);
        }
        MOD_INC_USE_COUNT;
        list_add(&type->mft_list, &mds_fs_types);

        return 0;
}

void mds_unregister_fs_type(const char *name)
{
        struct list_head *p;

        /* lock mds_fs_types list */
        list_for_each(p, &mds_fs_types) {
                struct mds_fs_type *type;

                type = list_entry(p, struct mds_fs_type, mft_list);
                if (!strcmp(type->mft_name, name)) {
                        list_del(p);
                        kfree(type->mft_name);
                        OBD_FREE(type, sizeof(*type));
                        MOD_DEC_USE_COUNT;
                        break;
                }
        }
        /* unlock mds_fs_types list */
}

int mds_fs_setup(struct mds_obd *mds, struct vfsmount *mnt)
{
        struct mds_fs_operations *fs_ops;
        int rc;

        if (!(fs_ops = mds_search_fs_type(mds->mds_fstype))) {
                char name[32];

                snprintf(name, sizeof(name) - 1, "mds_%s", mds->mds_fstype);
                name[sizeof(name) - 1] = '\0';

                if ((rc = request_module(name))) {
                        fs_ops = mds_search_fs_type(mds->mds_fstype);
                        CDEBUG(D_INFO, "Loaded module '%s'\n", name);
                        if (!fs_ops)
                                rc = -ENOENT;
                }

                if (rc) {
                        CERROR("Can't find MDS fs interface '%s'\n", name);
                        RETURN(rc);
                }
        }

        mds->mds_fsops = fs_ops;
        mds->mds_vfsmnt = mnt;

        OBD_SET_CTXT_MAGIC(&mds->mds_ctxt);
        mds->mds_ctxt.pwdmnt = mnt;
        mds->mds_ctxt.pwd = mnt->mnt_root;
        mds->mds_ctxt.fs = get_ds();

        /*
         * Replace the client filesystem delete_inode method with our own,
         * so that we can clear the object ID before the inode is deleted.
         * The fs_delete_inode method will call cl_delete_inode for us.
         * We need to do this for the MDS superblock only, hence we install
         * a modified copy of the original superblock method table.
         *
         * We still assume that there is only a single MDS client filesystem
         * type, as we don't have access to the mds struct in delete_inode
         * and store the client delete_inode method in a global table.  This
         * will only become a problem if/when multiple MDSs are running on a
         * single host with different underlying filesystems.
         */
        OBD_ALLOC(mds->mds_sop, sizeof(*mds->mds_sop));
        if (!mds->mds_sop)
                RETURN(-ENOMEM);

        memcpy(mds->mds_sop, mds->mds_sb->s_op, sizeof(*mds->mds_sop));
        mds->mds_fsops->cl_delete_inode = mds->mds_sop->delete_inode;
        mds->mds_sop->delete_inode = mds->mds_fsops->fs_delete_inode;
        mds->mds_sb->s_op = mds->mds_sop;

        rc = mds_fs_prep(mds);

        if (rc)
                OBD_FREE(mds->mds_sop, sizeof(*mds->mds_sop));

        return rc;
}

void mds_fs_cleanup(struct mds_obd *mds)
{
        mds_client_free_all(mds);
        mds_server_free_data(mds);

        OBD_FREE(mds->mds_sop, sizeof(*mds->mds_sop));
}

EXPORT_SYMBOL(mds_register_fs_type);
EXPORT_SYMBOL(mds_unregister_fs_type);
