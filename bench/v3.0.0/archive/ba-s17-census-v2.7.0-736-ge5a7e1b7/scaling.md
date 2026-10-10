| shape | durability | cores | engine | 1 session | 2 sessions | 4 sessions | 8 sessions | 16 sessions |
|---|---|---|---|---|---|---|---|---|
| trade | group | 1 | B | 819 stmt/s | 873 stmt/s | 1,687 stmt/s | 3,020 stmt/s | 5,569 stmt/s |
| trade | group | 1 | S4 | 676 stmt/s | 712 stmt/s | 1,368 stmt/s | 2,622 stmt/s | 4,977 stmt/s |
| trade | group | 2 | B | 792 stmt/s | 915 stmt/s | 1,781 stmt/s | 3,418 stmt/s | 6,606 stmt/s |
| trade | group | 2 | S4 | 657 stmt/s | 722 stmt/s | 811 stmt/s | 1,862 stmt/s | 1,656 stmt/s |
| trade | group | 4 | B | 793 stmt/s | 856 stmt/s | 1,747 stmt/s | 3,428 stmt/s | 6,793 stmt/s |
| trade | group | 4 | S4 | 654 stmt/s | 720 stmt/s | 1,434 stmt/s | 2,125 stmt/s | 3,905 stmt/s |
| trade | strict | 1 | B | 808 stmt/s | 860 stmt/s | 842 stmt/s | 874 stmt/s | 862 stmt/s |
| trade | strict | 1 | S4 | 680 stmt/s | 695 stmt/s | 700 stmt/s | 690 stmt/s | 684 stmt/s |
| trade | strict | 2 | B | 825 stmt/s | 926 stmt/s | 1,635 stmt/s | 3,026 stmt/s | 6,239 stmt/s |
| trade | strict | 2 | S4 | 653 stmt/s | 682 stmt/s | 717 stmt/s | 729 stmt/s | 721 stmt/s |
| trade | strict | 4 | B | 813 stmt/s | 887 stmt/s | 1,619 stmt/s | 3,156 stmt/s | 5,767 stmt/s |
| trade | strict | 4 | S4 | 655 stmt/s | 717 stmt/s | 1,000 stmt/s | 1,133 stmt/s | 857 stmt/s |
| trade | PostgreSQL on | CPUs 0 | BA-S4 | 673 stmt/s | 709 stmt/s | 1,313 stmt/s | 2,529 stmt/s | 4,902 stmt/s |
| trade | PostgreSQL on | CPUs 0,2 | BA-S4 | 678 stmt/s | 743 stmt/s | 1,416 stmt/s | 2,661 stmt/s | 5,150 stmt/s |
| insert1 | group | 1 | B | 851 stmt/s | 861 stmt/s | 1,687 stmt/s | 3,237 stmt/s | 5,921 stmt/s |
| insert1 | group | 1 | S4 | 668 stmt/s | 704 stmt/s | 1,346 stmt/s | 2,675 stmt/s | 4,927 stmt/s |
| insert1 | group | 2 | B | 842 stmt/s | 915 stmt/s | 1,467 stmt/s | 2,494 stmt/s | 6,208 stmt/s |
| insert1 | group | 2 | S4 | 671 stmt/s | 744 stmt/s | 825 stmt/s | 1,294 stmt/s | 2,922 stmt/s |
| insert1 | group | 4 | B | 819 stmt/s | 926 stmt/s | 1,808 stmt/s | 3,418 stmt/s | 6,328 stmt/s |
| insert1 | group | 4 | S4 | 682 stmt/s | 752 stmt/s | 1,104 stmt/s | 2,125 stmt/s | 3,991 stmt/s |
| insert1 | strict | 1 | B | 809 stmt/s | 844 stmt/s | 872 stmt/s | 892 stmt/s | 873 stmt/s |
| insert1 | strict | 1 | S4 | 708 stmt/s | 721 stmt/s | 733 stmt/s | 712 stmt/s | 728 stmt/s |
| insert1 | strict | 2 | B | 828 stmt/s | 946 stmt/s | 1,552 stmt/s | 3,189 stmt/s | 6,214 stmt/s |
| insert1 | strict | 2 | S4 | 702 stmt/s | 726 stmt/s | 739 stmt/s | 746 stmt/s | 703 stmt/s |
| insert1 | strict | 4 | B | 834 stmt/s | 915 stmt/s | 1,701 stmt/s | 3,085 stmt/s | 6,014 stmt/s |
| insert1 | strict | 4 | S4 | 665 stmt/s | 696 stmt/s | 942 stmt/s | 1,175 stmt/s | 1,010 stmt/s |
| insert1 | PostgreSQL on | CPUs 0 | BA-S4 | 708 stmt/s | 734 stmt/s | 1,385 stmt/s | 2,698 stmt/s | 5,120 stmt/s |
| insert1 | PostgreSQL on | CPUs 0,2 | BA-S4 | 691 stmt/s | 749 stmt/s | 1,448 stmt/s | 2,771 stmt/s | 5,325 stmt/s |
| insertN | group | 1 | B | 848 stmt/s | 862 stmt/s | 1,710 stmt/s | 3,310 stmt/s | 5,801 stmt/s |
| insertN | group | 1 | S4 | 701 stmt/s | 718 stmt/s | 1,376 stmt/s | 2,708 stmt/s | 4,918 stmt/s |
| insertN | group | 2 | B | 834 stmt/s | 934 stmt/s | 1,518 stmt/s | 3,218 stmt/s | 6,102 stmt/s |
| insertN | group | 2 | S4 | 689 stmt/s | 717 stmt/s | 1,072 stmt/s | 1,441 stmt/s | 3,558 stmt/s |
| insertN | group | 4 | B | 799 stmt/s | 908 stmt/s | 1,759 stmt/s | 3,324 stmt/s | 6,552 stmt/s |
| insertN | group | 4 | S4 | 687 stmt/s | 690 stmt/s | 1,440 stmt/s | 2,156 stmt/s | 4,226 stmt/s |
| insertN | strict | 1 | B | 854 stmt/s | 882 stmt/s | 903 stmt/s | 876 stmt/s | 861 stmt/s |
| insertN | strict | 1 | S4 | 688 stmt/s | 729 stmt/s | 744 stmt/s | 740 stmt/s | 726 stmt/s |
| insertN | strict | 2 | B | 836 stmt/s | 913 stmt/s | 1,650 stmt/s | 3,136 stmt/s | 6,383 stmt/s |
| insertN | strict | 2 | S4 | 706 stmt/s | 715 stmt/s | 767 stmt/s | 756 stmt/s | 752 stmt/s |
| insertN | strict | 4 | B | 841 stmt/s | 930 stmt/s | 1,729 stmt/s | 3,196 stmt/s | 6,077 stmt/s |
| insertN | strict | 4 | S4 | 692 stmt/s | 747 stmt/s | 1,008 stmt/s | 1,126 stmt/s | 985 stmt/s |
| insertN | PostgreSQL on | CPUs 0 | BA-S4 | 647 stmt/s | 702 stmt/s | 1,364 stmt/s | 2,622 stmt/s | 4,857 stmt/s |
| insertN | PostgreSQL on | CPUs 0,2 | BA-S4 | 686 stmt/s | 733 stmt/s | 1,403 stmt/s | 2,666 stmt/s | 5,035 stmt/s |
| recent | group | 1 | B | 1,540 stmt/s | 1,633 stmt/s | 2,196 stmt/s | 4,110 stmt/s | 7,226 stmt/s |
| recent | group | 1 | S4 | 1,274 stmt/s | 1,356 stmt/s | 1,777 stmt/s | 3,361 stmt/s | 6,033 stmt/s |
| recent | group | 2 | B | 1,521 stmt/s | 1,797 stmt/s | 3,558 stmt/s | 6,114 stmt/s | 12,102 stmt/s |
| recent | group | 2 | S4 | 1,253 stmt/s | 1,350 stmt/s | 2,094 stmt/s | 2,788 stmt/s | 6,325 stmt/s |
| recent | group | 4 | B | 1,496 stmt/s | 1,817 stmt/s | 3,525 stmt/s | 6,792 stmt/s | 12,627 stmt/s |
| recent | group | 4 | S4 | 1,250 stmt/s | 1,437 stmt/s | 2,789 stmt/s | 4,846 stmt/s | 7,376 stmt/s |
| recent | strict | 1 | B | 1,474 stmt/s | 1,573 stmt/s | 1,687 stmt/s | 1,719 stmt/s | 1,708 stmt/s |
| recent | strict | 1 | S4 | 1,248 stmt/s | 1,337 stmt/s | 1,371 stmt/s | 1,372 stmt/s | 1,336 stmt/s |
| recent | strict | 2 | B | 1,520 stmt/s | 1,685 stmt/s | 2,871 stmt/s | 5,727 stmt/s | 11,382 stmt/s |
| recent | strict | 2 | S4 | 1,248 stmt/s | 1,330 stmt/s | 1,376 stmt/s | 1,326 stmt/s | 1,472 stmt/s |
| recent | strict | 4 | B | 1,502 stmt/s | 1,672 stmt/s | 3,020 stmt/s | 5,667 stmt/s | 10,102 stmt/s |
| recent | strict | 4 | S4 | 1,252 stmt/s | 1,349 stmt/s | 1,814 stmt/s | 1,867 stmt/s | 1,867 stmt/s |
| recent | PostgreSQL on | CPUs 0 | BA-S4 | 1,232 stmt/s | 1,417 stmt/s | 2,466 stmt/s | 5,143 stmt/s | 9,733 stmt/s |
| recent | PostgreSQL on | CPUs 0,2 | BA-S4 | 1,238 stmt/s | 1,420 stmt/s | 2,702 stmt/s | 5,058 stmt/s | 9,942 stmt/s |
